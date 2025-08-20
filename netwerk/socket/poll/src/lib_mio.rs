/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#![expect(
    clippy::missing_safety_doc,
    clippy::missing_panics_doc,
    reason = "FIXME"
)]

use std::{collections::HashSet, ffi::c_void, os::fd::RawFd, ptr, time::Duration};

use mio::{event::Event, unix::SourceFd, Events, Interest, Token};
use thin_vec::ThinVec;

pub struct Poll {
    poll: mio::Poll,
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
    error: bool,
}

impl From<&Event> for PollEvent {
    fn from(event: &Event) -> Self {
        Self {
            token: event.token().0 as *mut c_void,
            readable: event.is_readable(),
            writable: event.is_writable(),
            priority: event.is_priority(),
            error: event.is_error(),
        }
    }
}

impl From<PollEvent> for Interest {
    fn from(event: PollEvent) -> Self {
        match (event.readable, event.writable) {
            (true, true) => Self::READABLE | Self::WRITABLE,
            (true, false) => Self::READABLE,
            (false, true) => Self::WRITABLE,
            (false, false) => unreachable!(),
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
    mio::Poll::new().map_or(ptr::null_mut(), |poll| {
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
        match poll.poll.registry().register(
            &mut SourceFd(&fd),
            Token(event.token as usize),
            event.into(),
        ) {
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
    let res = match poll.poll.registry().reregister(
        &mut SourceFd(&fd),
        Token(event.token as usize),
        event.into(),
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
    let res = match poll.poll.registry().deregister(&mut SourceFd(&fd)) {
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

    let mut poll = unsafe { Box::from_raw(poll) };
    let events_out = unsafe { &mut *events_out };
    events_out.clear();

    let timeout = if timeout_ms < 0 {
        None
    } else {
        Some(Duration::from_millis(
            u64::try_from(timeout_ms).expect("guaranteed to be positive"),
        ))
    };

    let mut events = Events::with_capacity(poll.set.len());
    let res = match poll.poll.poll(&mut events, timeout) {
        Ok(()) => {
            for event in &events {
                events_out.push(PollEvent::from(event));
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
