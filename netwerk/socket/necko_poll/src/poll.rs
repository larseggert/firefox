/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! POSIX `poll()` fallback backend for tier 3 platforms (Solaris, illumos, etc.).
//!
//! This uses the same `poll()` syscall that `PR_Poll` used on all Unix
//! platforms. It serves as the fallback for platforms where neither kqueue nor
//! epoll is available.
//!
//! Registration, deletion, and rekeying are purely userspace `HashMap`
//! operations with zero syscalls. The `pollfds` array is rebuilt from the
//! `HashMap` before each `poll()` call.
//!
//! A non-blocking pipe pair provides cross-thread notification. An
//! `AtomicBool` coalesces multiple `notify()` calls so that at most one
//! pipe write occurs between consecutive `wait()` calls.

use std::{
    io,
    os::fd::{AsRawFd, OwnedFd},
};

use rustc_hash::{FxBuildHasher, FxHashMap};
use thin_vec::ThinVec;

use crate::{
    Backend, Deadline, Fd, FdState, PR_POLL_ERR, PR_POLL_EXCEPT, PR_POLL_HUP, PollEvent,
    create_nonblocking_pipe, drain_pipe, timeout_to_i32,
};

pub struct Poller {
    fd_state: FxHashMap<Fd, FdState>,
    pollfds: Vec<libc::pollfd>,
    fd_order: Vec<Fd>,
    notify_read: OwnedFd,
    notify_write: OwnedFd,
}

impl Backend for Poller {
    fn fd_state(&self) -> &FxHashMap<Fd, FdState> {
        &self.fd_state
    }

    fn fd_state_mut(&mut self) -> &mut FxHashMap<Fd, FdState> {
        &mut self.fd_state
    }

    fn new() -> Option<Self> {
        let (notify_read, notify_write) = create_nonblocking_pipe()?;

        Some(Self {
            fd_state: FxHashMap::with_capacity_and_hasher(32, FxBuildHasher),
            pollfds: Vec::new(),
            fd_order: Vec::new(),
            notify_read,
            notify_write,
        })
    }

    #[expect(
        clippy::iter_over_hash_type,
        reason = "iteration order is irrelevant for building the pollfds array"
    )]
    fn wait(&mut self, timeout_ms: i64, events: &mut ThinVec<PollEvent>) -> io::Result<()> {
        self.pollfds.clear();
        self.fd_order.clear();

        self.pollfds.push(libc::pollfd {
            fd: self.notify_read.as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        });

        for (&fd, state) in &self.fd_state {
            let mut poll_events: i16 = 0;
            if state.readable {
                poll_events |= libc::POLLIN;
            }
            if state.writable {
                poll_events |= libc::POLLOUT;
            }
            if state.priority {
                poll_events |= libc::POLLPRI;
            }
            self.pollfds.push(libc::pollfd {
                fd: fd.0,
                events: poll_events,
                revents: 0,
            });
            self.fd_order.push(fd);
        }

        let deadline = Deadline::new(timeout_ms);
        let mut remaining = timeout_ms;
        loop {
            let ret = unsafe {
                libc::poll(
                    self.pollfds.as_mut_ptr(),
                    self.pollfds.len() as libc::nfds_t,
                    timeout_to_i32(remaining),
                )
            };
            if ret >= 0 {
                break;
            }
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::Interrupted {
                match deadline.remaining_ms() {
                    Some(ms) => {
                        remaining = ms;
                        continue;
                    }
                    None => return Ok(()),
                }
            }
            return Err(e);
        }

        events.reserve(self.fd_order.len());

        for (i, fd) in self.fd_order.iter().enumerate() {
            let revents = self.pollfds[i + 1].revents;
            if revents == 0 {
                continue;
            }

            let Some(state) = self.fd_state.get(fd) else {
                debug_assert!(false, "fd in fd_order but not in fd_state");
                continue;
            };

            let readable = revents & libc::POLLIN != 0;
            let writable = revents & libc::POLLOUT != 0;

            let mut extra = 0;
            if state.priority && revents & libc::POLLPRI != 0 {
                extra |= PR_POLL_EXCEPT;
            }
            if revents & libc::POLLERR != 0 {
                extra |= PR_POLL_ERR | PR_POLL_EXCEPT;
            }
            if revents & libc::POLLHUP != 0 {
                extra |= PR_POLL_HUP;
            }

            if let Some(poll_event) = state.to_poll_event(readable, writable, extra) {
                events.push(poll_event);
            }
        }

        Ok(())
    }

    fn drain_notification(&self) {
        drain_pipe(&self.notify_read);
    }

    fn send_notify(&self) -> io::Result<()> {
        rustix::io::write(&self.notify_write, &[1u8])?;
        Ok(())
    }
}
