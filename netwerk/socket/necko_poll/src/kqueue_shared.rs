/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! Shared changelist helpers for the macOS and BSD kqueue backends.

use std::{collections::hash_map::Entry, io, os::fd::RawFd};

use rustc_hash::FxHashMap;

use crate::{Fd, FdState};

/// Implemented by kqueue changelist entry types, allowing [`register`] and
/// [`delete`] to be shared between the macOS (`libc::kevent64_s`) and BSD
/// (`kq::Event`) backends.
pub trait ChangeEntry: Sized {
    fn make(fd: RawFd, filter: i16, flags: u16, udata: isize) -> Self;
}

pub fn register<E: ChangeEntry>(
    changelist: &mut Vec<E>,
    fd_state: &mut FxHashMap<Fd, FdState>,
    fd: Fd,
    state: FdState,
) {
    let raw_fd = fd.0;
    let udata = raw_fd as isize;
    match fd_state.entry(fd) {
        Entry::Occupied(mut entry) => {
            let old = *entry.get();
            if old.same_interest(&state) {
                let e = entry.get_mut();
                e.user_key = state.user_key;
                e.mapping = state.mapping;
                return;
            }
            if old.readable && !state.readable {
                changelist.push(E::make(raw_fd, libc::EVFILT_READ, libc::EV_DELETE, 0));
            }
            if old.writable && !state.writable {
                changelist.push(E::make(raw_fd, libc::EVFILT_WRITE, libc::EV_DELETE, 0));
            }
            if state.readable && !old.readable {
                changelist.push(E::make(raw_fd, libc::EVFILT_READ, libc::EV_ADD, udata));
            }
            if state.writable && !old.writable {
                changelist.push(E::make(raw_fd, libc::EVFILT_WRITE, libc::EV_ADD, udata));
            }
            entry.insert(state);
        }
        Entry::Vacant(entry) => {
            if state.readable {
                changelist.push(E::make(raw_fd, libc::EVFILT_READ, libc::EV_ADD, udata));
            }
            if state.writable {
                changelist.push(E::make(raw_fd, libc::EVFILT_WRITE, libc::EV_ADD, udata));
            }
            entry.insert(state);
        }
    }
}

pub fn delete<E: ChangeEntry>(
    changelist: &mut Vec<E>,
    fd_state: &mut FxHashMap<Fd, FdState>,
    fd: Fd,
) -> io::Result<()> {
    let state = fd_state.remove(&fd).ok_or(io::ErrorKind::NotFound)?;
    let raw_fd = fd.0;
    if state.readable {
        changelist.push(E::make(raw_fd, libc::EVFILT_READ, libc::EV_DELETE, 0));
    }
    if state.writable {
        changelist.push(E::make(raw_fd, libc::EVFILT_WRITE, libc::EV_DELETE, 0));
    }
    Ok(())
}
