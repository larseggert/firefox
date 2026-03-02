/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! Platform-native I/O readiness polling for Firefox's socket transport service.
//!
//! This crate replaces NSPR's `PR_Poll` with the most efficient polling mechanism
//! available on each platform. Where `PR_Poll` used `select()` on macOS and Windows and POSIX `poll()` on other Unix
//! targets, this crate uses kqueue on macOS/BSDs, epoll
//! on Linux/Android, and `select()` on Windows, each driven directly via platform
//! syscalls with no intermediate abstraction layer. It exposes a C FFI consumed by
//! `nsSocketTransportService` to monitor socket readiness with minimal syscall cost
//! per poll iteration.
//!
//! # Design
//!
//! Each platform backend implements the [`Backend`] trait, providing `register`,
//! `delete`, `rekey`, `wait`, and `notify` operations. The FFI layer in this module
//! delegates to a compile-time selected `PlatformPoller` type alias.
//!
//! ## fd-as-key
//!
//! The kernel-stored event key is always the raw file descriptor (or `SOCKET` on
//! Windows), not the caller's `SocketContext*` pointer. This decouples kernel state
//! from userspace bookkeeping: when the C++ side swap-removes a `SocketContext` and
//! needs to update its index (`poll_rekey`), only a `HashMap` update is needed with
//! zero syscalls. Interest-set changes are detected by diffing `(readable, writable,
//! priority)` so a rekey with unchanged interest is always free.
//!
//! ## Event mapping
//!
//! NSS layers can request that a logical "read ready" be satisfied by the socket
//! becoming writable (e.g. during a TLS handshake that needs to send data before a
//! read can complete). [`EventMapping`] captures this per-fd relationship and
//! translates raw kernel events back into `PR_POLL_READ`/`PR_POLL_WRITE` flags.
//!
//! ## Cross-thread notification
//!
//! All backends provide a `notify()` method that wakes a blocked `wait()` from
//! another thread, used by `nsSocketTransportService::OnDispatchedEvent`. The
//! mechanism is platform-specific (eventfd, `EVFILT_USER`, pipe, or loopback UDP),
//! but all share `AtomicBool` coalescing so that at most one wake syscall occurs
//! between consecutive `wait()` calls regardless of how many threads call `notify()`.
//!
//! # Platform backends
//!
//! | Platform | Backend | Syscalls per iteration |
//! |---|---|---|
//! | macOS | kqueue + `EVFILT_MACHPORT` | 1 `kevent64()` |
//! | iOS, FreeBSD, `DragonFly` BSD | kqueue + `EVFILT_USER` | 1 `kevent()` |
//! | OpenBSD, NetBSD | kqueue + pipe | 1 `kevent()` |
//! | Linux, Android | epoll with native timeout | 1 `epoll_wait()` |
//! | Windows | `select()` | 1 syscall |
//! | Solaris, other Unix | POSIX `poll()` | 1 `poll()` |
//!
//! # Safety
//!
//! The FFI functions are `unsafe extern "C"` and require valid pointers from the C++
//! caller. Internally, each backend uses `unsafe` only for platform syscalls and
//! `BorrowedFd::borrow_raw` on fds whose lifetime is managed by the C++ side.

#[cfg(not(windows))]
use std::os::fd::RawFd;
use std::{
    io, ptr,
    sync::atomic::{AtomicBool, Ordering},
};

use rustc_hash::FxHashMap;
use thin_vec::ThinVec;
#[cfg(windows)]
use windows_sys::Win32::Networking::WinSock::SOCKET;

#[cfg(epoll)]
mod epoll;
#[cfg(bsd_kqueue)]
mod kqueue;
#[cfg(kqueue)]
mod kqueue_shared;
#[cfg(target_os = "macos")]
mod macos;
#[cfg(poll)]
mod poll;
#[cfg(windows)]
mod windows;

#[cfg(not(windows))]
#[repr(transparent)]
#[derive(Eq, Hash, PartialEq, Debug, Clone, Copy)]
pub struct Fd(RawFd);
#[cfg(windows)]
#[repr(transparent)]
#[derive(Eq, Hash, PartialEq, Debug, Clone, Copy)]
pub struct Fd(SOCKET);

const PR_POLL_READ: i16 = 0x1;
const PR_POLL_WRITE: i16 = 0x2;
const PR_POLL_EXCEPT: i16 = 0x4;
const PR_POLL_ERR: i16 = 0x8;
#[cfg(not(windows))]
const PR_POLL_HUP: i16 = 0x40;

/// Maps system-level poll events (readable/writable) to `PR_POLL` flags.
///
/// NSS layers may request that `PR_POLL_READ` be satisfied by `POLLOUT`
/// (e.g., during TLS handshake when a read requires sending data).
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct EventMapping {
    read_from_read: bool,
    write_from_read: bool,
    read_from_write: bool,
    write_from_write: bool,
}

impl EventMapping {
    const fn compute(
        wants_read: bool,
        wants_write: bool,
        poll_flags_for_read: i16,
        poll_flags_for_write: i16,
    ) -> Self {
        // No fallback when the layer poll returns 0 for a direction.
        // This matches PR_Poll's behavior: if the layer doesn't want
        // kernel events for that direction, don't register for them.
        Self {
            read_from_read: wants_read && (poll_flags_for_read & PR_POLL_READ) != 0,
            write_from_read: wants_write && (poll_flags_for_write & PR_POLL_READ) != 0,
            read_from_write: wants_read && (poll_flags_for_read & PR_POLL_WRITE) != 0,
            write_from_write: wants_write && (poll_flags_for_write & PR_POLL_WRITE) != 0,
        }
    }

    const fn readable(self) -> bool {
        self.read_from_read || self.write_from_read
    }

    const fn writable(self) -> bool {
        self.read_from_write || self.write_from_write
    }

    const fn translate(self, readable: bool, writable: bool) -> i16 {
        let mut flags: i16 = 0;
        if (readable && self.read_from_read) || (writable && self.read_from_write) {
            flags |= PR_POLL_READ;
        }
        if (readable && self.write_from_read) || (writable && self.write_from_write) {
            flags |= PR_POLL_WRITE;
        }
        flags
    }
}

/// Per-fd state stored in the backend's `HashMap`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FdState {
    user_key: usize,
    mapping: EventMapping,
    readable: bool,
    writable: bool,
    priority: bool,
}

impl FdState {
    #[cfg(any(kqueue, epoll))]
    const fn same_interest(&self, other: &Self) -> bool {
        self.readable == other.readable
            && self.writable == other.writable
            && self.priority == other.priority
    }

    fn to_poll_event(self, readable: bool, writable: bool, extra_flags: i16) -> Option<PollEvent> {
        let flags = self.mapping.translate(readable, writable) | extra_flags;
        (flags != 0).then_some(PollEvent {
            key: self.user_key,
            flags,
        })
    }
}

trait Backend {
    fn new() -> Option<Self>
    where
        Self: Sized;
    fn fd_state(&self) -> &FxHashMap<Fd, FdState>;
    fn fd_state_mut(&mut self) -> &mut FxHashMap<Fd, FdState>;
    fn wait(&mut self, timeout_ms: i64, events: &mut ThinVec<PollEvent>) -> io::Result<()>;
    fn send_notify(&self) -> io::Result<()>;

    /// Drain any pending wakeup token without calling `wait()`. Called by
    /// `consume_notified` after clearing the `AtomicBool` flag, to remove
    /// the kernel-level token so the next `wait()` doesn't return
    /// immediately for a stale notification. The default no-op is used by
    /// `EVFILT_USER` backends where `EV_CLEAR` auto-drains on delivery.
    fn drain_notification(&self) {}

    fn register(&mut self, fd: Fd, state: FdState) -> io::Result<()> {
        self.fd_state_mut().insert(fd, state);
        Ok(())
    }

    fn delete(&mut self, fd: Fd) -> io::Result<()> {
        self.fd_state_mut()
            .remove(&fd)
            .ok_or(io::ErrorKind::NotFound)?;
        Ok(())
    }

    fn rekey(&mut self, fd: Fd, new_key: usize) -> io::Result<()> {
        let entry = self
            .fd_state_mut()
            .get_mut(&fd)
            .ok_or(io::ErrorKind::NotFound)?;
        entry.user_key = new_key;
        Ok(())
    }

    fn len(&self) -> usize {
        self.fd_state().len()
    }
}

#[cfg(any(poll, pipe_notify))]
fn create_nonblocking_pipe() -> Option<(std::os::fd::OwnedFd, std::os::fd::OwnedFd)> {
    use rustix::fs::{OFlags, fcntl_getfl, fcntl_setfl};
    let (r, w) = rustix::pipe::pipe().ok()?;
    fcntl_setfl(&r, fcntl_getfl(&r).ok()? | OFlags::NONBLOCK).ok()?;
    fcntl_setfl(&w, fcntl_getfl(&w).ok()? | OFlags::NONBLOCK).ok()?;
    Some((r, w))
}

#[cfg(any(poll, pipe_notify))]
fn drain_pipe(fd: &std::os::fd::OwnedFd) {
    let mut buf = [0u8; 64];
    while rustix::io::read(fd, &mut buf).unwrap_or(0) > 0 {}
}

#[cfg(any(epoll, poll))]
fn timeout_to_i32(timeout_ms: i64) -> i32 {
    if timeout_ms < 0 {
        -1
    } else {
        i32::try_from(timeout_ms).unwrap_or(i32::MAX)
    }
}

/// Tracks a deadline for EINTR retry loops.
///
/// `PR_Poll` (`_pr_poll_with_poll` in ptio.c) retries the `poll()` syscall on
/// EINTR with the remaining timeout. Without this, signal delivery to the
/// socket thread (profiler sampling, timers) would cause the wait to return
/// early and trigger a full re-iteration of `DoPollIteration` — redundantly
/// re-polling all NSPR layers and recomputing timeouts. This helper
/// replicates `PR_Poll`'s retry strategy: non-blocking calls (timeout=0)
/// return immediately, infinite timeouts retry unconditionally, and timed
/// waits retry with the remaining duration.
#[cfg(not(windows))]
enum Deadline {
    NonBlocking,
    Infinite,
    At(std::time::Instant),
}

#[cfg(not(windows))]
impl Deadline {
    fn new(timeout_ms: i64) -> Self {
        match timeout_ms {
            0 => Self::NonBlocking,
            t if t < 0 => Self::Infinite,
            t => Self::At(
                std::time::Instant::now() + std::time::Duration::from_millis(t.cast_unsigned()),
            ),
        }
    }

    /// Returns the remaining timeout in milliseconds for a retry, or `None`
    /// if the deadline has passed and the caller should return with 0 events.
    fn remaining_ms(&self) -> Option<i64> {
        match self {
            Self::NonBlocking => None,
            Self::Infinite => Some(-1),
            Self::At(deadline) => {
                let now = std::time::Instant::now();
                #[expect(
                    clippy::cast_possible_truncation,
                    reason = "timeouts are always well within i64 range"
                )]
                (now < *deadline).then(|| (*deadline - now).as_millis() as i64)
            }
        }
    }
}

#[cfg(target_os = "macos")]
type PlatformPoller = macos::Poller;
#[cfg(bsd_kqueue)]
type PlatformPoller = kqueue::Poller;
#[cfg(epoll)]
type PlatformPoller = epoll::Poller;
#[cfg(windows)]
type PlatformPoller = windows::Poller;
#[cfg(poll)]
type PlatformPoller = poll::Poller;

pub struct Poller {
    inner: PlatformPoller,
    notified: AtomicBool,
}

impl Poller {
    fn notify(&self) -> io::Result<()> {
        if self.notified.swap(true, Ordering::AcqRel) {
            return Ok(());
        }
        self.inner.send_notify()
    }
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
    flags: i16,
}

#[unsafe(no_mangle)]
pub extern "C" fn necko_poll_new() -> *mut Poller {
    PlatformPoller::new().map_or(ptr::null_mut(), |inner| {
        Box::into_raw(Box::new(Poller {
            inner,
            notified: AtomicBool::new(false),
        }))
    })
}

/// # Safety
/// `poll` must be a valid pointer returned from `poll_new`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_free(poll: *mut Poller) {
    if poll.is_null() {
        return;
    }
    unsafe {
        _ = Box::from_raw(poll);
    }
}

/// # Safety
/// `poll` must be a valid, non-null pointer returned from `poll_new`.
/// `fd` must be a valid file descriptor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_register(
    poll: *mut Poller,
    fd: Fd,
    wants_read: bool,
    wants_write: bool,
    wants_except: bool,
    poll_flags_for_read: i16,
    poll_flags_for_write: i16,
    key: usize,
) -> PollResult {
    let poll = unsafe { &mut *poll };

    let mapping = EventMapping::compute(
        wants_read,
        wants_write,
        poll_flags_for_read,
        poll_flags_for_write,
    );
    let state = FdState {
        user_key: key,
        mapping,
        readable: mapping.readable(),
        writable: mapping.writable(),
        priority: wants_except,
    };

    match poll.inner.register(fd, state) {
        Ok(()) => PollResult::Ok,
        Err(_) => PollResult::ErrorIo,
    }
}

/// # Safety
/// `poll` must be a valid, non-null pointer returned from `poll_new`.
/// `fd` must be a currently registered file descriptor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_rekey(poll: *mut Poller, fd: Fd, new_key: usize) -> PollResult {
    let poll = unsafe { &mut *poll };

    match poll.inner.rekey(fd, new_key) {
        Ok(()) => PollResult::Ok,
        Err(_) => PollResult::ErrorInvalidArg,
    }
}

/// # Safety
/// `poll` must be a valid, non-null pointer returned from `poll_new`.
/// `fd` must be a currently registered file descriptor.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_delete(poll: *mut Poller, fd: Fd) -> PollResult {
    let poll = unsafe { &mut *poll };

    match poll.inner.delete(fd) {
        Ok(()) => PollResult::Ok,
        Err(_) => PollResult::ErrorIo,
    }
}

/// # Safety
/// `poll` must be a valid, non-null pointer returned from `poll_new`.
/// `events_out` must be a valid, non-null pointer to a `ThinVec<PollEvent>`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_wait(
    poll: *mut Poller,
    events_out: *mut ThinVec<PollEvent>,
    timeout_ms: i64,
) -> i32 {
    let events_out = unsafe { &mut *events_out };
    events_out.clear();

    let poll = unsafe { &mut *poll };

    match poll.inner.wait(timeout_ms, events_out) {
        Ok(()) => i32::try_from(events_out.len()).unwrap_or(i32::MAX),
        Err(_) => -1,
    }
}

/// # Safety
/// `poll` must be a valid, non-null pointer returned from `poll_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_len(poll: *const Poller) -> usize {
    let poll = unsafe { &*poll };
    poll.inner.len()
}

/// Wakes the socket thread if it is blocked in `wait()`.
///
/// # Safety
/// `poll` must be a valid, non-null pointer returned from `poll_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_notify(poll: *const Poller) {
    let poll = unsafe { &*poll };
    _ = poll.notify();
}

/// Checks and clears the cross-thread notification flag.
///
/// Returns true if a notification was pending. Drains the underlying
/// platform wakeup token (eventfd, pipe, Mach message) so the next
/// `wait()` won't return immediately for a stale notification.
///
/// # Safety
/// `poll` must be a valid, non-null pointer returned from `poll_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn necko_poll_consume_notified(poll: *const Poller) -> bool {
    let poll = unsafe { &*poll };
    let was_notified = poll.notified.swap(false, Ordering::AcqRel);
    if was_notified {
        poll.inner.drain_notification();
    }
    was_notified
}
