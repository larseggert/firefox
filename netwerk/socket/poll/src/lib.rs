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
    collections::HashSet,
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

pub struct Poller {
    poll: polling::Poller,
    set: HashSet<Fd>,
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
    polling::Poller::new().map_or(ptr::null_mut(), |poll| {
        assert!(
            poll.supports_level(),
            "Level-triggered polling not supported"
        );
        Box::into_raw(Box::new(Poller {
            poll,
            set: HashSet::default(),
        }))
    })
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

fn do_poll_add(fd: Fd, poll: &mut Poller, event: PollEvent) -> PollResult {
    assert!(
        !poll.set.contains(&fd),
        "poll_add: {fd:?} already registered"
    );

    eprintln!(
        "poll_add: fd={:?} readable={} writable={} priority={}",
        fd, event.readable, event.writable, event.priority
    );

    match unsafe {
        let Ok(event) = event.try_into() else {
            return PollResult::ErrorInvalidArg;
        };
        #[cfg(not(windows))]
        let source = BorrowedFd::borrow_raw(fd.0);
        #[cfg(windows)]
        let source = BorrowedSocket::borrow_raw(fd.0);
        poll.poll.add_with_mode(&source, event, PollMode::Level)
    } {
        Ok(()) => {
            poll.set.insert(fd);
            eprintln!("poll_add: success, set_len={}", poll.set.len());
            PollResult::Ok
        }
        Err(e) => {
            eprintln!("poll_add {fd:?} error: {e}");
            PollResult::ErrorIo
        }
    }
}

// Add a file descriptor to the poller.
#[no_mangle]
pub unsafe extern "C" fn poll_add(poll: *mut Poller, fd: Fd, event: PollEvent) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }
    // eprintln!("poll_add: fd={:?} readable={} writable={} priority={}", fd, event.readable, event.writable, event.priority);

    let mut poll = unsafe { Box::from_raw(poll) };
    let res = do_poll_add(fd, &mut poll, event);
    _ = Box::into_raw(poll);
    res
}

// Modify an existing file descriptor in the poll
#[no_mangle]
pub unsafe extern "C" fn poll_modify(poll: *mut Poller, fd: Fd, event: PollEvent) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }

    let mut poll = unsafe { Box::from_raw(poll) };
    // assert!(
    //     poll.set.contains(&fd),
    //     "poll_modify: {fd:?} not registered, have {:?}",
    //     poll.set
    // );
    let res = if poll.set.contains(&fd) {
        let Ok(event) = event.try_into() else {
            _ = Box::into_raw(poll);
            return PollResult::ErrorInvalidArg;
        };
        #[cfg(not(windows))]
        let source = BorrowedFd::borrow_raw(fd.0);
        #[cfg(windows)]
        let source = BorrowedSocket::borrow_raw(fd.0);
        match poll.poll.modify_with_mode(source, event, PollMode::Level) {
            Ok(()) => PollResult::Ok,
            Err(e) => {
                eprintln!("poll_modify {fd:?} error: {e}");
                PollResult::ErrorIo
            }
        }
    } else {
        eprintln!(
            "poll_modify: {fd:?} not registered, have {:?}, adding",
            poll.set
        );
        let res = do_poll_add(fd, &mut poll, event);
        eprintln!("poll_modify: now have {:?}", poll.set);
        res
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
    // eprintln!("poll_delete: {fd:?}");

    let mut poll = unsafe { Box::from_raw(poll) };
    assert!(poll.set.contains(&fd), "poll_delete: {fd:?} not registered");
    #[cfg(not(windows))]
    let source = BorrowedFd::borrow_raw(fd.0);
    #[cfg(windows)]
    let source = BorrowedSocket::borrow_raw(fd.0);
    let res = match poll.poll.delete(source) {
        Ok(()) => {
            poll.set.remove(&fd);
            PollResult::Ok
        }
        Err(e) => {
            eprintln!("poll_delete {fd:?} error: {e}");
            PollResult::ErrorIo
        }
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
        eprintln!("poll_wait: null pointer");
        return -1;
    }

    let events_out = unsafe { &mut *events_out };
    events_out.clear();

    let poll = unsafe { Box::from_raw(poll) };
    let Some(capacity) = NonZeroUsize::new(poll.set.len()) else {
        eprintln!("poll_wait: zero fds registered");
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

    eprintln!(
        "poll_wait: capacity={} timeout_ms={} calling wait...",
        capacity, timeout_ms
    );

    let mut events = Events::with_capacity(capacity);
    let res = match poll.poll.wait(&mut events, timeout) {
        Ok(_) => {
            eprintln!("poll_wait: wait returned Ok, {} events", events.len());
            for event in events.iter() {
                eprintln!(
                    "poll_wait: event key={} readable={} writable={}",
                    event.key, event.readable, event.writable
                );
                let Ok(event) = event.try_into() else {
                    _ = Box::into_raw(poll);
                    return -1;
                };
                events_out.push(event);
            }
            isize::try_from(events_out.len()).unwrap_or(isize::MAX)
        }
        Err(e) => {
            eprintln!("poll_wait: wait returned error: {}", e);
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
