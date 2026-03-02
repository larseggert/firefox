/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! macOS backend using kqueue with `EVFILT_MACHPORT` cross-thread notification.
//!
//! On macOS, `nsSocketTransportService` runs in the parent process or the
//! privileged socket process, neither of which is subject to the `kevent()`
//! sandbox restrictions that apply to content processes.
//!
//! `PR_Poll` used POSIX `poll()` on macOS, which requires the kernel to scan
//! the entire fd array on every call. kqueue is an O(1)-per-event notification
//! mechanism that avoids this linear scan entirely.
//!
//! Following the pattern established by nginx, libuv, and libevent, all
//! registration and deletion changes are accumulated in a changelist and
//! flushed in a single `kevent64()` syscall together with the wait, achieving
//! one syscall per poll iteration regardless of how many fds changed interest
//! between iterations.
//!
//! Level-triggered mode is used (no `EV_CLEAR` on socket filters) to match
//! Firefox's event processing model, where readiness must persist across
//! multiple poll cycles until the socket is actually serviced.
//!
//! Cross-thread notification uses `EVFILT_MACHPORT` with inline Mach message
//! receive (`MACH_RCV_MSG` in `fflags`). This is significantly faster than
//! `EVFILT_USER`, especially when triggered across threads, as the Mach IPC
//! fast path in XNU is more optimized than the BSD kevent path for cross-thread
//! signaling. The message is received inline within the `kevent64()` call,
//! saving a separate `mach_msg_receive()` syscall per wakeup. An `AtomicBool`
//! coalesces multiple `notify()` calls into at most one in-flight Mach message
//! between consecutive `wait()` calls.
//!
//! The changelist and eventlist use `libc::kevent64_s`, required by
//! `kevent64()`. The `ext[0]`/`ext[1]` fields carry the inline receive buffer
//! pointer and size for `EVFILT_MACHPORT`.

use std::{
    io,
    os::fd::{AsRawFd as _, OwnedFd, RawFd},
    ptr,
};

use rustc_hash::{FxBuildHasher, FxHashMap};
use thin_vec::ThinVec;

use crate::kqueue_shared::{self, ChangeEntry};
use crate::{Backend, Deadline, Fd, FdState, PR_POLL_ERR, PR_POLL_EXCEPT, PR_POLL_HUP, PollEvent};

/// `kevent64()` flags value requesting immediate return after processing
/// the changelist without waiting for events.
const KEVENT_FLAG_IMMEDIATE: libc::c_uint = 0x001;

/// Empty Mach message for sending a wakeup signal via `mach_msg_send`.
#[repr(C)]
struct MachMsgEmptySend {
    header: mach2::message::mach_msg_header_t,
}

/// Receive buffer for a Mach message received inline via `EVFILT_MACHPORT`.
#[repr(C)]
struct MachMsgEmptyRcv {
    header: mach2::message::mach_msg_header_t,
    trailer: mach2::message::mach_msg_trailer_t,
}

pub struct Poller {
    kq_fd: OwnedFd,
    changelist: Vec<libc::kevent64_s>,
    eventlist: Vec<libc::kevent64_s>,
    fd_state: FxHashMap<Fd, FdState>,
    wakeup_port: mach2::port::mach_port_t,
    /// Heap-allocated receive buffer whose address remains stable after the
    /// Poller is moved into a `Box`, as required by the `EVFILT_MACHPORT`
    /// kevent registration that stores a pointer to this buffer.
    #[expect(
        dead_code,
        reason = "held for its heap allocation lifetime; address was registered with kqueue at construction"
    )]
    wakeup_buffer: Box<MachMsgEmptyRcv>,
}

impl ChangeEntry for libc::kevent64_s {
    #[expect(
        clippy::cast_sign_loss,
        reason = "file descriptor values are non-negative"
    )]
    fn make(fd: RawFd, filter: i16, flags: u16, udata: isize) -> Self {
        Self {
            ident: fd as u64,
            filter,
            flags,
            fflags: 0,
            data: 0,
            udata: udata as u64,
            ext: [0, 0],
        }
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

        let mut wakeup_port: mach2::port::mach_port_t = mach2::port::MACH_PORT_NULL;
        let kr = unsafe {
            mach2::mach_port::mach_port_allocate(
                mach2::traps::mach_task_self(),
                mach2::port::MACH_PORT_RIGHT_RECEIVE,
                &raw mut wakeup_port,
            )
        };
        if kr != mach2::kern_return::KERN_SUCCESS {
            return None;
        }

        // Heap-allocate the receive buffer so its address is stable
        // after the Poller is moved into a Box.
        let wakeup_buffer = Box::new(MachMsgEmptyRcv {
            header: mach2::message::mach_msg_header_t::default(),
            trailer: mach2::message::mach_msg_trailer_t::default(),
        });

        let ev = libc::kevent64_s {
            ident: u64::from(wakeup_port),
            filter: libc::EVFILT_MACHPORT,
            flags: libc::EV_ADD,
            // MACH_RCV_MSG causes the kernel to receive the pending Mach
            // message inline into ext[0]/ext[1] when the event fires,
            // avoiding a separate mach_msg_receive() syscall.
            fflags: mach2::message::MACH_RCV_MSG as u32,
            data: 0,
            udata: 0,
            ext: [
                (&raw const *wakeup_buffer) as u64,
                size_of::<MachMsgEmptyRcv>() as u64,
            ],
        };
        let ret = unsafe {
            libc::kevent64(
                kq_fd.as_raw_fd(),
                &raw const ev,
                1,
                ptr::null_mut(),
                0,
                KEVENT_FLAG_IMMEDIATE,
                ptr::null(),
            )
        };
        if ret < 0 {
            return None;
        }

        Some(Self {
            kq_fd,
            changelist: Vec::with_capacity(64),
            eventlist: Vec::with_capacity(64),
            fd_state: FxHashMap::with_capacity_and_hasher(32, FxBuildHasher),
            wakeup_port,
            wakeup_buffer,
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

        let mut timeout_spec;
        let mut timeout_ptr: *const libc::timespec = if timeout_ms < 0 {
            ptr::null()
        } else {
            timeout_spec = libc::timespec {
                tv_sec: timeout_ms / 1000,
                tv_nsec: (timeout_ms % 1000) * 1_000_000,
            };
            &raw const timeout_spec
        };

        let nchanges = i32::try_from(self.changelist.len()).unwrap_or(i32::MAX);
        let nevents = i32::try_from(self.eventlist.capacity()).unwrap_or(i32::MAX);

        let deadline = Deadline::new(timeout_ms);
        let n = loop {
            let n = unsafe {
                libc::kevent64(
                    self.kq_fd.as_raw_fd(),
                    self.changelist.as_ptr(),
                    nchanges,
                    self.eventlist.as_mut_ptr(),
                    nevents,
                    0,
                    timeout_ptr,
                )
            };
            if n >= 0 {
                break n;
            }
            let e = io::Error::last_os_error();
            // Flush the changelist on the first attempt so retries don't
            // re-submit already-applied changes.
            self.changelist.clear();
            if e.kind() == io::ErrorKind::Interrupted {
                match deadline.remaining_ms() {
                    Some(ms) => {
                        timeout_spec = libc::timespec {
                            tv_sec: ms / 1000,
                            tv_nsec: (ms % 1000) * 1_000_000,
                        };
                        timeout_ptr = &raw const timeout_spec;
                        continue;
                    }
                    None => return Ok(()),
                }
            }
            return Err(e);
        };
        self.changelist.clear();
        // SAFETY: `kevent64` wrote exactly `n` valid entries, and n >=
        // 0 <= capacity was verified above.
        unsafe {
            self.eventlist.set_len(n.unsigned_abs() as usize);
        }
        events.reserve(self.eventlist.len());

        for ev in &self.eventlist {
            if ev.flags & libc::EV_ERROR != 0 {
                continue;
            }

            if ev.filter == libc::EVFILT_MACHPORT {
                continue;
            }

            let Ok(raw_fd) = i32::try_from(ev.udata) else {
                continue;
            };
            let fd = Fd(raw_fd);
            let Some(state) = self.fd_state.get(&fd) else {
                continue;
            };

            let readable = ev.filter == libc::EVFILT_READ;
            let writable = ev.filter == libc::EVFILT_WRITE;

            let mut extra = 0;
            if ev.flags & libc::EV_EOF != 0 {
                extra |= PR_POLL_HUP;
                if ev.fflags != 0 {
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

    #[expect(
        clippy::cast_possible_truncation,
        reason = "MachMsgEmptySend is 24 bytes, well within u32"
    )]
    fn drain_notification(&self) {
        // Non-blocking receive to drain a stale Mach message from the
        // port. EVFILT_MACHPORT is level-triggered; unlike EVFILT_USER,
        // EV_CLEAR doesn't prevent re-triggering while a message is
        // queued. Draining the message ensures the next kevent64 won't
        // return immediately for a stale wakeup.
        let mut buf = MachMsgEmptyRcv {
            header: mach2::message::mach_msg_header_t::default(),
            trailer: mach2::message::mach_msg_trailer_t::default(),
        };
        unsafe {
            mach2::message::mach_msg(
                &raw mut buf.header,
                mach2::message::MACH_RCV_MSG | mach2::message::MACH_RCV_TIMEOUT,
                0,
                size_of::<MachMsgEmptyRcv>() as mach2::message::mach_msg_size_t,
                self.wakeup_port,
                0, // timeout = 0 (non-blocking)
                mach2::port::MACH_PORT_NULL,
            );
            // Ignore errors — MACH_RCV_TIMED_OUT means no message was
            // pending, which is fine.
        }
    }

    #[expect(
        clippy::cast_possible_truncation,
        reason = "MachMsgEmptySend is 24 bytes, well within u32"
    )]
    fn send_notify(&self) -> io::Result<()> {
        let mut msg = MachMsgEmptySend {
            header: mach2::message::mach_msg_header_t {
                msgh_bits: mach2::message::MACH_MSGH_BITS(
                    mach2::message::MACH_MSG_TYPE_MAKE_SEND_ONCE,
                    0,
                ),
                msgh_size: size_of::<MachMsgEmptySend>() as mach2::message::mach_msg_size_t,
                msgh_remote_port: self.wakeup_port,
                msgh_local_port: mach2::port::MACH_PORT_NULL,
                msgh_voucher_port: mach2::port::MACH_PORT_NULL,
                msgh_id: 0,
            },
        };
        let kr = unsafe { mach2::message::mach_msg_send(&raw mut msg.header) };
        if kr != mach2::message::MACH_MSG_SUCCESS {
            // Port queue is full or transient error; a wakeup is already pending.
            unsafe {
                mach2::message::mach_msg_destroy(&raw mut msg.header);
            }
        }
        Ok(())
    }
}

impl Drop for Poller {
    fn drop(&mut self) {
        unsafe {
            mach2::mach_port::mach_port_mod_refs(
                mach2::traps::mach_task_self(),
                self.wakeup_port,
                mach2::port::MACH_PORT_RIGHT_RECEIVE,
                -1,
            );
        }
    }
}
