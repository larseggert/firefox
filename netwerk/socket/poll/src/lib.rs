/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#![expect(
    clippy::missing_safety_doc,
    clippy::missing_panics_doc,
    reason = "FIXME"
)]

use std::{
    collections::HashSet,
    ffi::c_void,
    num::NonZeroUsize,
    os::fd::{BorrowedFd, RawFd},
    ptr,
    time::Duration,
};

use polling::{Event, Events, Poller};
use thin_vec::ThinVec;

pub struct Poll {
    poll: Poller,
    set: HashSet<RawFd>,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PollResult {
    Ok,
    ErrorInvalidArg,
    ErrorIo,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct PollEvent {
    token: *mut c_void,
    readable: bool,
    writable: bool,
    priority: bool,
    interrupt: bool,
    error: bool,
}

impl From<&Event> for PollEvent {
    fn from(event: &Event) -> Self {
        Self {
            token: event.key as *mut c_void,
            readable: event.readable,
            writable: event.writable,
            priority: event.is_priority(),
            interrupt: event.is_interrupt(),
            error: event.is_err().unwrap_or(false),
        }
    }
}

impl From<PollEvent> for Event {
    fn from(event: PollEvent) -> Self {
        let e = Self::new(event.token as usize, event.readable, event.writable);
        let e = if event.priority { e.with_priority() } else { e };
        if event.interrupt {
            e.with_interrupt()
        } else {
            e
        }
    }
}

impl PollEvent {
    #[no_mangle]
    pub const extern "C" fn poll_event_new(
        token: *mut c_void,
        readable: bool,
        writable: bool,
    ) -> Self {
        Self {
            token,
            readable,
            writable,
            priority: false,
            interrupt: false,
            error: false,
        }
    }

    #[no_mangle]
    pub const extern "C" fn poll_event_new_readable(token: *mut c_void) -> Self {
        Self::poll_event_new(token, true, false)
    }
}

// Create a new poll instance
#[no_mangle]
pub extern "C" fn poll_new() -> *mut Poll {
    Poller::new().map_or(ptr::null_mut(), |poll| {
        assert!(
            poll.supports_edge(),
            "Edge-triggered polling is not supported"
        );
        Box::into_raw(Box::new(Poll {
            poll,
            set: HashSet::default(),
        }))
    })
}

// Destroy a poll instance
#[no_mangle]
pub unsafe extern "C" fn poll_destroy(poll: *mut Poll) {
    if poll.is_null() {
        return;
    }
    unsafe {
        _ = Box::from_raw(poll);
    }
}

// Add a file descriptor to the ;pol
#[no_mangle]
pub unsafe extern "C" fn poll_add(poll: *mut Poll, fd: RawFd, event: PollEvent) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }
    // eprintln!("poll_add: Adding file descriptor {fd}");

    let mut poll = unsafe { Box::from_raw(poll) };
    let res = if poll.set.contains(&fd) {
        // eprintln!("poll_add: File descriptor {fd} already registered");
        PollResult::Ok
    } else {
        match unsafe {
            poll.poll
                .add_with_mode(fd, event.into(), polling::PollMode::Edge)
        } {
            Ok(()) => {
                poll.set.insert(fd);
                PollResult::Ok
            }
            Err(_) => PollResult::ErrorIo,
        }
    };
    _ = Box::into_raw(poll);
    res
}

// Modify an existing file descriptor in the poll
#[no_mangle]
pub unsafe extern "C" fn poll_modify(poll: *mut Poll, fd: RawFd, event: PollEvent) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }

    let poll = unsafe { Box::from_raw(poll) };
    assert!(
        poll.set.contains(&fd),
        "File descriptor {fd} not registered"
    );
    let res = match poll.poll.modify_with_mode(
        unsafe { BorrowedFd::borrow_raw(fd) },
        event.into(),
        polling::PollMode::Edge,
    ) {
        Ok(()) => PollResult::Ok,
        Err(_) => PollResult::ErrorIo,
    };
    _ = Box::into_raw(poll);
    res
}

// Remove a file descriptor from the poll
#[no_mangle]
pub unsafe extern "C" fn poll_delete(poll: *mut Poll, fd: RawFd) -> PollResult {
    if poll.is_null() {
        return PollResult::ErrorInvalidArg;
    }
    // eprintln!("poll_delete: Removing file descriptor {fd}");

    let mut poll = unsafe { Box::from_raw(poll) };
    assert!(
        poll.set.contains(&fd),
        "File descriptor {fd} not registered"
    );
    let res = match poll.poll.delete(unsafe { BorrowedFd::borrow_raw(fd) }) {
        Ok(()) => {
            poll.set.remove(&fd);
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
    poll: *mut Poll,
    events_out: *mut ThinVec<PollEvent>,
    timeout_ms: i64,
) -> isize {
    if poll.is_null() || events_out.is_null() {
        return -1;
    }

    let poll = unsafe { Box::from_raw(poll) };
    let Some(capacity) = NonZeroUsize::new(poll.set.len()) else {
        return -1;
    };

    let events_out = unsafe { &mut *events_out };
    events_out.clear();

    let timeout = if timeout_ms < 0 {
        None
    } else {
        Some(Duration::from_millis(
            u64::try_from(timeout_ms).expect("guaranteed to be positive"),
        ))
    };

    let mut events = Events::with_capacity(capacity);
    let res = match poll.poll.wait(&mut events, timeout) {
        Ok(n) => {
            for event in events.iter() {
                events_out.push(PollEvent::from(&event));
            }
            isize::try_from(n).unwrap_or(isize::MAX)
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
