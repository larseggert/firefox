/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#![expect(
    clippy::missing_safety_doc,
    clippy::missing_panics_doc,
    reason = "FIXME"
)]

#[cfg(not(windows))]
use std::os::fd::{BorrowedFd, RawFd};
#[cfg(windows)]
use std::os::windows::{io::BorrowedSocket, raw::SOCKET};
use std::{
    collections::{HashMap, HashSet},
    num::{NonZeroUsize, TryFromIntError},
    ptr,
    time::Duration,
};

use polling::{Event, Events, PollMode};
use thin_vec::ThinVec;

#[cfg(not(windows))]
#[repr(transparent)]
#[derive(Eq, Hash, PartialEq, Debug, Clone, Copy)]
pub struct Fd(RawFd);
#[cfg(windows)]
#[repr(transparent)]
#[derive(Eq, Hash, PartialEq, Debug, Clone, Copy)]
pub struct Fd(SOCKET);

/// Mapping that tracks how system poll events should be translated to PR events.
/// This mimics PR_Poll's _PR_POLL_READ_SYS_READ, _PR_POLL_READ_SYS_WRITE, etc.
#[derive(Default, Clone, Copy)]
struct EventMapping {
    /// POLLIN should report PR_POLL_READ
    read_from_read: bool,
    /// POLLIN should report PR_POLL_WRITE
    write_from_read: bool,
    /// POLLOUT should report PR_POLL_READ
    read_from_write: bool,
    /// POLLOUT should report PR_POLL_WRITE
    write_from_write: bool,
}

pub struct Poller {
    poll: polling::Poller,
    set: HashSet<Fd>,
    /// Per-fd mapping for translating system events to PR events
    mappings: HashMap<Fd, EventMapping>,
}

#[repr(C)]
pub enum PollResult {
    Ok,
    ErrorInvalidArg,
    ErrorIo,
}

#[repr(C)]
#[derive(Debug)]
pub struct PollEvent {
    key: usize,
    readable: bool,
    writable: bool,
    priority: bool,
    error: bool,
}

impl TryFrom<Event> for PollEvent {
    type Error = TryFromIntError;

    fn try_from(value: Event) -> Result<Self, Self::Error> {
        Ok(Self {
            key: value.key,
            readable: value.readable,
            writable: value.writable,
            priority: value.is_priority(),
            error: value.is_err().unwrap_or(false),
        })
    }
}

impl TryFrom<PollEvent> for Event {
    type Error = TryFromIntError;

    fn try_from(value: PollEvent) -> Result<Self, Self::Error> {
        let e = Self::new(value.key, value.readable, value.writable);
        let e = if value.priority { e.with_priority() } else { e };
        Ok(e)
    }
}

impl PollEvent {
    #[no_mangle]
    #[must_use]
    pub const extern "C" fn poll_event_new(key: usize, readable: bool, writable: bool) -> Self {
        Self {
            key,
            readable,
            writable,
            priority: false,
            error: false,
        }
    }

    #[no_mangle]
    #[must_use]
    pub const extern "C" fn poll_event_new_readable(key: usize) -> Self {
        Self::poll_event_new(key, true, false)
    }

    #[no_mangle]
    #[must_use]
    pub const extern "C" fn poll_event_new_writable(key: usize) -> Self {
        Self::poll_event_new(key, false, true)
    }
}

// Create a new poll instance
#[no_mangle]
pub extern "C" fn poll_new() -> *mut Poller {
    match polling::Poller::new() {
        Ok(poll) => Box::into_raw(Box::new(Poller {
            poll,
            set: HashSet::default(),
            mappings: HashMap::default(),
        })),
        Err(_) => ptr::null_mut(),
    }
}

// Free a poll instance
#[no_mangle]
pub unsafe extern "C" fn poll_free(poll: *mut Poller) {
    if poll.is_null() {
        return;
    }
    unsafe {
        _ = Box::from_raw(poll);
    }
}

fn do_poll_add(
    fd: Fd,
    poll: &mut Poller,
    readable: bool,
    writable: bool,
    priority: bool,
) -> PollResult {
    assert!(
        !poll.set.contains(&fd),
        "poll_add: {fd:?} already registered"
    );

    let event = Event::new(fd.0 as usize, readable, writable);
    let event = if priority {
        event.with_priority()
    } else {
        event
    };
    match unsafe {
        #[cfg(not(windows))]
        let source = BorrowedFd::borrow_raw(fd.0);
        #[cfg(windows)]
        let source = BorrowedSocket::borrow_raw(fd.0);
        poll.poll.add_with_mode(&source, event, PollMode::Level)
    } {
        Ok(()) => {
            poll.set.insert(fd);
            PollResult::Ok
        }
        Err(_) => PollResult::ErrorIo,
    }
}

// Add a file descriptor to the poller.
#[no_mangle]
pub unsafe extern "C" fn poll_add(
    poll: *mut Poller,
    fd: Fd,
    readable: bool,
    writable: bool,
) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }

    let mut poll = unsafe { Box::from_raw(poll) };
    let res = do_poll_add(fd, &mut poll, readable, writable, false);
    _ = Box::into_raw(poll);
    res
}

// Modify an existing file descriptor in the poll with event translation mapping.
// - wants_read/wants_write/wants_except: what the application requested
// - poll_flags_read: return value from layer->poll when called with PR_POLL_READ
// - poll_flags_write: return value from layer->poll when called with PR_POLL_WRITE
// The mapping is computed from layer poll results and used to translate system
// events back to application events in poll_wait.
#[no_mangle]
pub unsafe extern "C" fn poll_modify(
    poll: *mut Poller,
    fd: Fd,
    wants_read: bool,
    wants_write: bool,
    wants_except: bool,
    poll_flags_read: i16,
    poll_flags_write: i16,
) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }

    // NSPR poll flag constants (stable ABI)
    const POLL_READ: i16 = 0x1;
    const POLL_WRITE: i16 = 0x2;

    let mut poll = unsafe { Box::from_raw(poll) };

    // Compute the event mapping from layer poll results.
    // This matches PR_Poll's _PR_POLL_READ_SYS_READ, etc. mapping.
    // - poll_flags_read: what to poll for when application wants READ
    // - poll_flags_write: what to poll for when application wants WRITE
    //
    // Only use mappings for directions the app actually requested.
    // If app wants READ only, we shouldn't poll for POLLOUT (even if the layer
    // returned poll_flags_write with POLLOUT set) because we'd have nothing to
    // report if POLLOUT fires.
    let mut read_from_read = wants_read && (poll_flags_read & POLL_READ) != 0;
    let read_from_write = wants_read && (poll_flags_read & POLL_WRITE) != 0;
    let write_from_read = wants_write && (poll_flags_write & POLL_READ) != 0;
    let mut write_from_write = wants_write && (poll_flags_write & POLL_WRITE) != 0;

    // If layer returns 0 for poll flags for a requested direction, fall back
    // to identity mapping for that direction. This handles simple layers that
    // don't implement poll() and just want us to poll for what was requested.
    if wants_read && !read_from_read && !read_from_write {
        read_from_read = true;
    }
    if wants_write && !write_from_read && !write_from_write {
        write_from_write = true;
    }

    // Store the mapping for event translation in poll_wait
    poll.mappings.insert(
        fd,
        EventMapping {
            read_from_read,
            write_from_read,
            read_from_write,
            write_from_write,
        },
    );

    // Determine system events to poll for based on the mapping.
    // Poll for POLLIN if any mapping uses it, POLLOUT if any mapping uses it.
    let readable = read_from_read || write_from_read;
    let writable = read_from_write || write_from_write;

    let res = if poll.set.contains(&fd) {
        let event = Event::new(fd.0 as usize, readable, writable);
        let event = if wants_except {
            event.with_priority()
        } else {
            event
        };
        #[cfg(not(windows))]
        let source = BorrowedFd::borrow_raw(fd.0);
        #[cfg(windows)]
        let source = BorrowedSocket::borrow_raw(fd.0);
        match poll.poll.modify_with_mode(source, event, PollMode::Level) {
            Ok(()) => PollResult::Ok,
            Err(_) => PollResult::ErrorIo,
        }
    } else {
        do_poll_add(fd, &mut poll, readable, writable, wants_except)
    };
    _ = Box::into_raw(poll);
    res
}

// Remove a file descriptor from the poll
#[no_mangle]
pub unsafe extern "C" fn poll_delete(poll: *mut Poller, fd: Fd) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }

    let mut poll = unsafe { Box::from_raw(poll) };
    assert!(poll.set.contains(&fd), "poll_delete: {fd:?} not registered");
    #[cfg(not(windows))]
    let source = BorrowedFd::borrow_raw(fd.0);
    #[cfg(windows)]
    let source = BorrowedSocket::borrow_raw(fd.0);
    let res = match poll.poll.delete(source) {
        Ok(()) => {
            poll.set.remove(&fd);
            poll.mappings.remove(&fd);
            PollResult::Ok
        }
        Err(_) => PollResult::ErrorIo,
    };
    _ = Box::into_raw(poll);
    res
}

// Wait for events and populate the provided ThinVec (ABI-compatible with nsTArray)
// timeout_ms: -1 for infinite, 0 for non-blocking
// Returns the number of events populated, or -1 on error
#[no_mangle]
pub unsafe extern "C" fn poll_wait(
    poll: *mut Poller,
    events_out: *mut ThinVec<PollEvent>,
    timeout_ms: i64,
) -> isize {
    if poll.is_null() || events_out.is_null() {
        return -1;
    }

    let events_out = unsafe { &mut *events_out };
    events_out.clear();

    let poll = unsafe { Box::from_raw(poll) };
    let Some(capacity) = NonZeroUsize::new(poll.set.len()) else {
        _ = Box::into_raw(poll);
        return -1;
    };

    let timeout = if timeout_ms < 0 {
        None
    } else {
        Some(Duration::from_millis(
            u64::try_from(timeout_ms).expect("guaranteed to be positive"),
        ))
    };

    let mut events = Events::with_capacity(capacity);
    let res = match poll.poll.wait(&mut events, timeout) {
        Ok(_) => {
            for event in events.iter() {
                // Translate system events to PR events using the stored mapping
                #[cfg(not(windows))]
                let fd = Fd(event.key as RawFd);
                #[cfg(windows)]
                let fd = Fd(event.key as SOCKET);

                let (readable, writable) = if let Some(mapping) = poll.mappings.get(&fd) {
                    let readable = (event.readable && mapping.read_from_read)
                        || (event.writable && mapping.read_from_write);
                    let writable = (event.readable && mapping.write_from_read)
                        || (event.writable && mapping.write_from_write);
                    (readable, writable)
                } else {
                    (event.readable, event.writable)
                };

                events_out.push(PollEvent {
                    key: event.key,
                    readable,
                    writable,
                    priority: event.is_priority(),
                    error: event.is_err().unwrap_or(false),
                });
            }
            isize::try_from(events_out.len()).unwrap_or(isize::MAX)
        }
        Err(e) => {
            if e.kind() == std::io::ErrorKind::TimedOut {
                0 // No events, but not an error
            } else {
                -1 // Error condition
            }
        }
    };
    _ = Box::into_raw(poll);
    res
}

#[no_mangle]
pub unsafe extern "C" fn poll_len(poll: *mut Poller) -> usize {
    if poll.is_null() {
        return 0;
    }
    let poll = unsafe { Box::from_raw(poll) };
    let len = poll.set.len();
    _ = Box::into_raw(poll);
    len
}
