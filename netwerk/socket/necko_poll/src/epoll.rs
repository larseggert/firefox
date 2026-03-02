/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! epoll backend for Linux and Android.
//!
//! `PR_Poll` used POSIX `poll()` on Linux, which requires the kernel to scan
//! the entire fd array on every call. epoll is an O(1)-per-event notification
//! mechanism that maintains a persistent interest set in the kernel, avoiding
//! this linear scan.
//!
//! Uses `epoll_wait()` with its native millisecond timeout parameter directly.
//! Registration changes are applied immediately via `epoll_ctl` since epoll
//! has no changelist batching mechanism (unlike kqueue), but the fd-as-key
//! design ensures that `rekey` (the most frequent operation) requires no
//! syscall at all. Interest-set diffing in `register` further avoids
//! `epoll_ctl` calls when only the userspace key or mapping changed.
//!
//! Cross-thread notification uses a single `eventfd` registered with
//! level-triggered `EPOLLIN`. An `AtomicBool` coalesces multiple `notify()`
//! calls so that at most one `write()` to the eventfd occurs between
//! consecutive `wait()` calls. The eventfd is drained with a single `read()`
//! only when an actual notification event is observed.

use std::{
    collections::hash_map::Entry,
    io,
    os::fd::{BorrowedFd, OwnedFd, RawFd},
};

use rustc_hash::{FxBuildHasher, FxHashMap};
use rustix::event::{EventfdFlags, epoll, eventfd};
use thin_vec::ThinVec;

use crate::{
    Backend, Deadline, Fd, FdState, PR_POLL_ERR, PR_POLL_EXCEPT, PR_POLL_HUP, PollEvent,
    timeout_to_i32,
};

pub struct Poller {
    epoll_fd: OwnedFd,
    notify_fd: OwnedFd,
    eventbuf: epoll::EventVec,
    fd_state: FxHashMap<Fd, FdState>,
}

impl Backend for Poller {
    fn fd_state(&self) -> &FxHashMap<Fd, FdState> {
        &self.fd_state
    }

    fn fd_state_mut(&mut self) -> &mut FxHashMap<Fd, FdState> {
        &mut self.fd_state
    }

    fn new() -> Option<Self> {
        let epoll_fd = epoll::create(epoll::CreateFlags::CLOEXEC).ok()?;
        let notify_fd = eventfd(0, EventfdFlags::CLOEXEC | EventfdFlags::NONBLOCK).ok()?;

        epoll::add(
            &epoll_fd,
            &notify_fd,
            epoll::EventData::new_u64(u64::MAX),
            epoll::EventFlags::IN,
        )
        .ok()?;

        Some(Self {
            epoll_fd,
            notify_fd,
            eventbuf: epoll::EventVec::with_capacity(64),
            fd_state: FxHashMap::with_capacity_and_hasher(32, FxBuildHasher),
        })
    }

    fn register(&mut self, fd: Fd, state: FdState) -> io::Result<()> {
        let mut flags = epoll::EventFlags::empty();
        if state.readable {
            flags |= epoll::EventFlags::IN;
        }
        if state.writable {
            flags |= epoll::EventFlags::OUT;
        }
        if state.priority {
            flags |= epoll::EventFlags::PRI;
        }

        let data = epoll::EventData::new_u64(fd.0 as u64);
        let source = unsafe { BorrowedFd::borrow_raw(fd.0) };

        match self.fd_state.entry(fd) {
            Entry::Occupied(mut entry) => {
                if entry.get().same_interest(&state) {
                    let e = entry.get_mut();
                    e.user_key = state.user_key;
                    e.mapping = state.mapping;
                    return Ok(());
                }
                epoll::modify(&self.epoll_fd, source, data, flags)?;
                entry.insert(state);
            }
            Entry::Vacant(entry) => {
                epoll::add(&self.epoll_fd, source, data, flags)?;
                entry.insert(state);
            }
        }
        Ok(())
    }

    fn delete(&mut self, fd: Fd) -> io::Result<()> {
        if !self.fd_state.contains_key(&fd) {
            return Err(io::ErrorKind::NotFound.into());
        }
        let source = unsafe { BorrowedFd::borrow_raw(fd.0) };
        epoll::delete(&self.epoll_fd, source)?;
        self.fd_state.remove(&fd);
        Ok(())
    }

    fn wait(&mut self, timeout_ms: i64, events: &mut ThinVec<PollEvent>) -> io::Result<()> {
        let required = self.fd_state.len() + 1;
        if self.eventbuf.capacity() < required {
            self.eventbuf.reserve(required - self.eventbuf.capacity());
        }

        let deadline = Deadline::new(timeout_ms);
        let mut remaining = timeout_ms;
        loop {
            match epoll::wait(
                &self.epoll_fd,
                &mut self.eventbuf,
                timeout_to_i32(remaining),
            ) {
                Ok(()) => break,
                Err(rustix::io::Errno::INTR) => match deadline.remaining_ms() {
                    Some(ms) => {
                        remaining = ms;
                        continue;
                    }
                    None => return Ok(()),
                },
                Err(e) => return Err(e.into()),
            }
        }
        events.reserve(self.eventbuf.len());

        for event in &self.eventbuf {
            let data = event.data.u64();

            if data == u64::MAX {
                continue;
            }

            let fd = Fd(data as RawFd);
            let Some(state) = self.fd_state.get(&fd) else {
                continue;
            };

            let eflags = event.flags;
            let readable = eflags.contains(epoll::EventFlags::IN);
            let writable = eflags.contains(epoll::EventFlags::OUT);

            let mut extra = 0;
            if state.priority && eflags.contains(epoll::EventFlags::PRI) {
                extra |= PR_POLL_EXCEPT;
            }
            if eflags.contains(epoll::EventFlags::ERR) {
                extra |= PR_POLL_ERR | PR_POLL_EXCEPT;
            }
            if eflags.intersects(epoll::EventFlags::HUP | epoll::EventFlags::RDHUP) {
                extra |= PR_POLL_HUP;
            }

            if let Some(poll_event) = state.to_poll_event(readable, writable, extra) {
                events.push(poll_event);
            }
        }

        Ok(())
    }

    fn drain_notification(&self) {
        // Read the eventfd to drain the pending counter, preventing
        // the next epoll_wait from returning immediately for a stale
        // notification.
        let mut buf = [0u8; 8];
        let _ = rustix::io::read(&self.notify_fd, &mut buf);
    }

    fn send_notify(&self) -> io::Result<()> {
        rustix::io::write(&self.notify_fd, &1u64.to_ne_bytes())?;
        Ok(())
    }
}
