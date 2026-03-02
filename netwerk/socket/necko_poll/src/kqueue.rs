/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! kqueue backend for iOS and BSDs (FreeBSD, OpenBSD, NetBSD, `DragonFly` BSD).
//!
//! On iOS, `kevent()` is blocked in sandboxed content processes (see
//! `be_kevent64` in `ipc/chromium/src/base/message_pump_kqueue.cc`).
//! `nsSocketTransportService` runs in the parent process or the socket
//! process (a privileged child), neither of which is subject to that
//! restriction, so `kevent()` is safe to use here.
//!
//! `PR_Poll` used POSIX `poll()` on these platforms, which requires the kernel
//! to scan the entire fd array on every call. kqueue is an O(1)-per-event
//! notification mechanism that avoids this linear scan entirely.
//!
//! Following the pattern established by nginx, libuv, and libevent, all
//! registration and deletion changes are accumulated in a changelist and
//! flushed in a single `kevent()` syscall together with the wait, achieving
//! one syscall per poll iteration regardless of how many fds changed interest
//! between iterations.
//!
//! Level-triggered mode is used (no `EV_CLEAR` on socket filters) to match
//! Firefox's event processing model, where readiness must persist across
//! multiple poll cycles until the socket is actually serviced.
//!
//! On FreeBSD and `DragonFly` BSD, cross-thread notification uses
//! `EVFILT_USER`. On OpenBSD and NetBSD, which lack `EVFILT_USER`, a
//! non-blocking pipe pair is used instead (gated on the `pipe_notify` cfg).

use std::{
    io,
    os::fd::{AsRawFd as _, OwnedFd, RawFd},
    time::Duration,
};

use rustc_hash::{FxBuildHasher, FxHashMap};
use rustix::event::kqueue as kq;
use thin_vec::ThinVec;

use crate::kqueue_shared::{self, ChangeEntry};
use crate::{Backend, Deadline, Fd, FdState, PR_POLL_ERR, PR_POLL_EXCEPT, PR_POLL_HUP, PollEvent};
#[cfg(pipe_notify)]
use crate::{create_nonblocking_pipe, drain_pipe};

const NOTIFY_IDENT: isize = -1;

/// Extract the `fflags` field from a kqueue event.
///
/// The rustix `kq::Event` type is `#[repr(transparent)]` over `libc::kevent`
/// but does not expose `fflags` for read/write filter events. Per kqueue(2),
/// `fflags` contains the socket error (e.g. `ECONNRESET`) when `EV_EOF` is
/// set, while `data` holds the byte count available to read/write.
const fn event_fflags(ev: &kq::Event) -> u32 {
    // SAFETY: `kq::Event` is `#[repr(transparent)]` over `libc::kevent`,
    // guaranteeing identical layout.
    let raw: &libc::kevent = unsafe { &*std::ptr::from_ref(ev).cast() };
    raw.fflags
}

pub struct Poller {
    kq_fd: OwnedFd,
    changelist: Vec<kq::Event>,
    eventlist: Vec<kq::Event>,
    fd_state: FxHashMap<Fd, FdState>,
    #[cfg(pipe_notify)]
    notify_read: OwnedFd,
    #[cfg(pipe_notify)]
    notify_write: OwnedFd,
}

impl ChangeEntry for kq::Event {
    fn make(fd: RawFd, filter: i16, flags: u16, udata: isize) -> Self {
        let kq_filter = if filter == libc::EVFILT_READ {
            kq::EventFilter::Read(fd)
        } else {
            kq::EventFilter::Write(fd)
        };
        kq::Event::new(kq_filter, kq::EventFlags::from_bits_retain(flags), udata)
    }
}

impl Backend for Poller {
    fn fd_state(&self) -> &FxHashMap<Fd, FdState> {
        &self.fd_state
    }

    fn fd_state_mut(&mut self) -> &mut FxHashMap<Fd, FdState> {
        &mut self.fd_state
    }

    fn new() -> Option<Self> {
        let kq_fd = rustix::event::kqueue::kqueue().ok()?;

        #[cfg(not(pipe_notify))]
        {
            let ev = kq::Event::new(
                kq::EventFilter::User {
                    ident: NOTIFY_IDENT,
                    flags: kq::UserFlags::NOINPUT,
                    user_flags: kq::UserDefinedFlags::new(0),
                },
                kq::EventFlags::ADD | kq::EventFlags::CLEAR,
                NOTIFY_IDENT,
            );
            let mut out = Vec::new();
            if unsafe { kq::kevent(&kq_fd, &[ev], &mut out, Some(Duration::ZERO)) }.is_err() {
                return None;
            }
        }

        #[cfg(pipe_notify)]
        let (notify_read, notify_write) = create_nonblocking_pipe()?;

        #[cfg_attr(
            not(pipe_notify),
            expect(unused_mut, reason = "only mutated under pipe_notify cfg")
        )]
        let mut changelist = Vec::with_capacity(64);
        #[cfg(pipe_notify)]
        changelist.push(kq::Event::new(
            kq::EventFilter::Read(notify_read.as_raw_fd()),
            kq::EventFlags::ADD | kq::EventFlags::CLEAR,
            NOTIFY_IDENT,
        ));

        Some(Self {
            kq_fd,
            changelist,
            eventlist: Vec::with_capacity(64),
            fd_state: FxHashMap::with_capacity_and_hasher(32, FxBuildHasher),
            #[cfg(pipe_notify)]
            notify_read,
            #[cfg(pipe_notify)]
            notify_write,
        })
    }

    fn register(&mut self, fd: Fd, state: FdState) -> io::Result<()> {
        kqueue_shared::register(&mut self.changelist, &mut self.fd_state, fd, state);
        Ok(())
    }

    fn delete(&mut self, fd: Fd) -> io::Result<()> {
        kqueue_shared::delete(&mut self.changelist, &mut self.fd_state, fd)
    }

    fn wait(&mut self, timeout_ms: i64, events: &mut ThinVec<PollEvent>) -> io::Result<()> {
        let min_cap = self.changelist.len() + self.fd_state.len() + 1;
        if self.eventlist.capacity() < min_cap {
            self.eventlist.reserve(min_cap - self.eventlist.capacity());
        }

        let deadline = Deadline::new(timeout_ms);
        let mut remaining = timeout_ms;
        loop {
            let t = u64::try_from(remaining).ok().map(Duration::from_millis);
            match unsafe { kq::kevent(&self.kq_fd, &self.changelist, &mut self.eventlist, t) } {
                Ok(()) => break,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {
                    self.changelist.clear();
                    match deadline.remaining_ms() {
                        Some(ms) => {
                            remaining = ms;
                            continue;
                        }
                        None => return Ok(()),
                    }
                }
                Err(e) => return Err(e),
            }
        }
        self.changelist.clear();
        events.reserve(self.eventlist.len());

        for ev in &self.eventlist {
            let flags = ev.flags();
            if flags.contains(kq::EventFlags::ERROR) {
                continue;
            }

            let udata = ev.udata();
            if udata == NOTIFY_IDENT {
                continue;
            }

            let Ok(raw_fd) = RawFd::try_from(udata) else {
                continue;
            };
            let fd = Fd(raw_fd);
            let Some(state) = self.fd_state.get(&fd) else {
                continue;
            };

            let filter = ev.filter();
            let readable = matches!(filter, kq::EventFilter::Read(_));
            let writable = matches!(filter, kq::EventFilter::Write(_));

            let mut extra = 0;
            if flags.contains(kq::EventFlags::EOF) {
                extra |= PR_POLL_HUP;
                if event_fflags(ev) != 0 {
                    extra |= PR_POLL_ERR;
                }
                if state.priority {
                    extra |= PR_POLL_EXCEPT;
                }
            }

            if let Some(poll_event) = state.to_poll_event(readable, writable, extra) {
                events.push(poll_event);
            }
        }

        Ok(())
    }

    #[cfg(pipe_notify)]
    fn drain_notification(&self) {
        drain_pipe(&self.notify_read);
    }

    fn send_notify(&self) -> io::Result<()> {
        #[cfg(not(pipe_notify))]
        {
            // Trigger the EVFILT_USER event. Use raw libc::kevent() with
            // nevents=0 to avoid allocating a throwaway output Vec.
            let ev = unsafe {
                libc::kevent {
                    ident: NOTIFY_IDENT as libc::uintptr_t,
                    filter: libc::EVFILT_USER,
                    flags: libc::EV_ADD | libc::EV_CLEAR,
                    fflags: libc::NOTE_TRIGGER,
                    data: 0,
                    udata: NOTIFY_IDENT as *mut libc::c_void,
                    ..std::mem::zeroed()
                }
            };
            let ret = unsafe {
                libc::kevent(
                    self.kq_fd.as_raw_fd(),
                    &raw const ev,
                    1,
                    std::ptr::null_mut(),
                    0,
                    std::ptr::null(),
                )
            };
            if ret < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }

        #[cfg(pipe_notify)]
        {
            rustix::io::write(&self.notify_write, &[1u8])?;
            Ok(())
        }
    }
}
