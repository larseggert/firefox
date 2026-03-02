/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! Windows backend using `select()` with an AF_UNIX socket pair for
//! cross-thread notification.
//!
//! This uses the same `select()` syscall that `PR_Poll` used on Windows (via
//! NSPR's `w32poll.c`). Unlike Unix platforms where kqueue or epoll provide a
//! clear upgrade over POSIX `poll()`, Windows `select()` outperforms `WSAPoll`
//! for Firefox's typically small active socket set.
//!
//! Registration is purely userspace. The three `fd_set` arrays (read, write,
//! except) are rebuilt before each `select()` call. The sets use a custom
//! `WinFdSet` with a 1024-entry capacity rather than the default `FD_SETSIZE`
//! of 64, matching NSPR's historical limit.
//!
//! Notification uses a connected AF_UNIX `SOCK_STREAM` socket pair. For
//! sandboxed child processes the parent creates the pair before launch and
//! passes the handles via geckoargs; the child stores them via
//! [`set_pre_notify_pair`] before [`Poller::new`] runs. For the parent process
//! the pair is created directly in [`Poller::new`].

use std::{
    io, ptr,
    sync::atomic::{AtomicUsize, Ordering},
};

use rustc_hash::{FxBuildHasher, FxHashMap};
use thin_vec::ThinVec;
use windows_sys::Win32::Networking::WinSock::{self as wsa, SOCKET};

use crate::{Backend, Fd, FdState, PR_POLL_ERR, PR_POLL_EXCEPT, PollEvent};

const WIN_FD_SETSIZE: usize = 1024;

#[repr(C)]
struct WinFdSet {
    fd_count: u32,
    fd_array: [SOCKET; WIN_FD_SETSIZE],
}

impl WinFdSet {
    fn new() -> Self {
        Self {
            fd_count: 0,
            fd_array: [0; WIN_FD_SETSIZE],
        }
    }

    fn clear(&mut self) {
        self.fd_count = 0;
    }

    fn add(&mut self, socket: SOCKET) {
        let count = self.fd_count as usize;
        debug_assert!(count < WIN_FD_SETSIZE, "WinFdSet overflow");
        if count < WIN_FD_SETSIZE {
            self.fd_array[count] = socket;
            self.fd_count += 1;
        }
    }

    fn contains(&self, socket: SOCKET) -> bool {
        let count = self.fd_count as usize;
        self.fd_array[..count].contains(&socket)
    }

    fn is_empty(&self) -> bool {
        self.fd_count == 0
    }

    fn as_mut_ptr(&mut self) -> *mut wsa::FD_SET {
        ptr::from_mut(self).cast()
    }
}

fn wsa_result(ret: i32) -> io::Result<()> {
    if ret == wsa::SOCKET_ERROR {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// `INVALID_SOCKET` as a `usize` sentinel for the pre-created pair atomics.
const INVALID: usize = wsa::INVALID_SOCKET;

/// Pre-created AF_UNIX notify pair, set by [`set_pre_notify_pair`] after
/// inheriting handles from the parent process, and consumed once by [`Poller::new`].
static PRE_NOTIFY_READ: AtomicUsize = AtomicUsize::new(INVALID);
static PRE_NOTIFY_WRITE: AtomicUsize = AtomicUsize::new(INVALID);

/// Creates a connected AF_UNIX `SOCK_STREAM` socket pair by emulating
/// `socketpair(2)`: binds a temporary listener, connects a client, accepts
/// the connection, then unlinks the temporary file.
///
/// Windows has no `socketpair()` and no autobind, so the listen/connect/accept
/// sequence is used instead.
pub(crate) fn create_af_unix_notify_pair() -> io::Result<(SOCKET, SOCKET)> {
    // WSAStartup may not have been called yet (we run before NSPR/networking
    // initialization). It is reference-counted and safe to call multiple times.
    let mut wsa_data: wsa::WSADATA = unsafe { std::mem::zeroed() };
    let rc = unsafe { wsa::WSAStartup(0x0202, &mut wsa_data) };
    if rc != 0 {
        return Err(io::Error::from_raw_os_error(rc));
    }

    static PATH_IDX: AtomicUsize = AtomicUsize::new(0);
    let idx = PATH_IDX.fetch_add(1, Ordering::Relaxed);

    let mut path = std::env::temp_dir();
    path.push(format!("necko-{}-{idx}.sock", std::process::id()));

    let path_str = path
        .to_str()
        .ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?;
    let path_bytes = path_str.as_bytes();

    let mut addr: wsa::SOCKADDR_UN = unsafe { std::mem::zeroed() };
    addr.sun_family = wsa::AF_UNIX;

    // sun_path must hold path_bytes plus a null terminator.
    if path_bytes.len() + 1 > addr.sun_path.len() {
        return Err(io::Error::from(io::ErrorKind::InvalidInput));
    }
    addr.sun_path[..path_bytes.len()].copy_from_slice(path_bytes);

    // Use the exact address length (not the full struct), matching what the
    // Windows AF_UNIX provider expects: sun_family + path bytes + null byte.
    let addr_len = (std::mem::offset_of!(wsa::SOCKADDR_UN, sun_path) + path_bytes.len() + 1) as i32;

    unsafe {
        let listener = wsa::socket(wsa::AF_UNIX as i32, wsa::SOCK_STREAM, 0);
        if listener == wsa::INVALID_SOCKET {
            return Err(io::Error::last_os_error());
        }

        let result = (|| -> io::Result<(SOCKET, SOCKET)> {
            wsa_result(wsa::bind(listener, ptr::from_ref(&addr).cast(), addr_len))?;
            wsa_result(wsa::listen(listener, 1))?;

            let client = wsa::socket(wsa::AF_UNIX as i32, wsa::SOCK_STREAM, 0);
            if client == wsa::INVALID_SOCKET {
                return Err(io::Error::last_os_error());
            }

            let inner = (|| -> io::Result<(SOCKET, SOCKET)> {
                wsa_result(wsa::connect(client, ptr::from_ref(&addr).cast(), addr_len))?;
                let server = wsa::accept(listener, ptr::null_mut(), ptr::null_mut());
                if server == wsa::INVALID_SOCKET {
                    return Err(io::Error::last_os_error());
                }
                // The file is only needed for the initial rendezvous; delete it
                // immediately so no temp file persists after process exit.
                let _ = std::fs::remove_file(&path);

                let nb_result = (|| -> io::Result<()> {
                    let mut nb: u32 = 1;
                    wsa_result(wsa::ioctlsocket(server, wsa::FIONBIO, &mut nb))?;
                    let mut nb: u32 = 1;
                    wsa_result(wsa::ioctlsocket(client, wsa::FIONBIO, &mut nb))
                })();

                match nb_result {
                    Ok(()) => Ok((server, client)),
                    Err(e) => {
                        wsa::closesocket(server);
                        Err(e)
                    }
                }
            })();

            if inner.is_err() {
                wsa::closesocket(client);
            }
            inner
        })();

        wsa::closesocket(listener);
        result
    }
}

/// Stores a pre-created AF_UNIX notify pair for later use by [`Poller::new`].
pub(crate) fn set_pre_notify_pair(read: SOCKET, write: SOCKET) {
    PRE_NOTIFY_READ.store(read, Ordering::Release);
    PRE_NOTIFY_WRITE.store(write, Ordering::Release);
}

fn take_pre_created_pair() -> Option<(SOCKET, SOCKET)> {
    let read = PRE_NOTIFY_READ.swap(INVALID, Ordering::AcqRel);
    let write = PRE_NOTIFY_WRITE.swap(INVALID, Ordering::AcqRel);
    if read == INVALID || write == INVALID {
        // Partially set state shouldn't happen; close any stray socket.
        unsafe {
            if read != INVALID {
                wsa::closesocket(read);
            }
            if write != INVALID {
                wsa::closesocket(write);
            }
        }
        return None;
    }
    Some((read, write))
}

fn drain_notify(socket: SOCKET) {
    let mut buf = [0u8; 512];
    unsafe {
        loop {
            let r = wsa::recv(socket, buf.as_mut_ptr().cast(), buf.len() as i32, 0);
            if r <= 0 {
                break;
            }
        }
    }
}

pub struct Poller {
    fd_state: FxHashMap<Fd, FdState>,
    /// Read end of the cross-thread notification socket pair.
    notify_read: SOCKET,
    /// Write end of the cross-thread notification socket pair.
    notify_write: SOCKET,
    read_set: WinFdSet,
    write_set: WinFdSet,
    except_set: WinFdSet,
}

impl Backend for Poller {
    fn fd_state(&self) -> &FxHashMap<Fd, FdState> {
        &self.fd_state
    }

    fn fd_state_mut(&mut self) -> &mut FxHashMap<Fd, FdState> {
        &mut self.fd_state
    }

    fn new() -> Option<Self> {
        let (notify_read, notify_write) =
            take_pre_created_pair().or_else(|| create_af_unix_notify_pair().ok())?;

        Some(Self {
            fd_state: FxHashMap::with_capacity_and_hasher(32, FxBuildHasher),
            notify_read,
            notify_write,
            read_set: WinFdSet::new(),
            write_set: WinFdSet::new(),
            except_set: WinFdSet::new(),
        })
    }

    #[expect(
        clippy::iter_over_hash_type,
        reason = "iteration order is irrelevant for building fd_sets"
    )]
    fn wait(&mut self, timeout_ms: i64, events: &mut ThinVec<PollEvent>) -> io::Result<()> {
        self.read_set.clear();
        self.write_set.clear();
        self.except_set.clear();

        self.read_set.add(self.notify_read);

        for (&fd, state) in &self.fd_state {
            if state.readable {
                self.read_set.add(fd.0);
            }
            if state.writable {
                self.write_set.add(fd.0);
            }
            if state.priority {
                self.except_set.add(fd.0);
            }
        }

        let timeout_val;
        let timeout_ptr = if timeout_ms < 0 {
            ptr::null()
        } else {
            timeout_val = wsa::TIMEVAL {
                tv_sec: i32::try_from(timeout_ms / 1000).unwrap_or(i32::MAX),
                tv_usec: (timeout_ms % 1000) as i32 * 1000,
            };
            &timeout_val
        };

        let n = unsafe {
            wsa::select(
                0,
                self.read_set.as_mut_ptr(),
                if self.write_set.is_empty() {
                    ptr::null_mut()
                } else {
                    self.write_set.as_mut_ptr()
                },
                if self.except_set.is_empty() {
                    ptr::null_mut()
                } else {
                    self.except_set.as_mut_ptr()
                },
                timeout_ptr,
            )
        };

        wsa_result(n)?;

        if n > 0 {
            events.reserve(self.fd_state.len());
            for (&fd, state) in &self.fd_state {
                let readable = self.read_set.contains(fd.0);
                let writable = self.write_set.contains(fd.0);
                let except = self.except_set.contains(fd.0);
                if !readable && !writable && !except {
                    continue;
                }
                let extra = if except {
                    PR_POLL_ERR | PR_POLL_EXCEPT
                } else {
                    0
                };
                if let Some(poll_event) = state.to_poll_event(readable, writable, extra) {
                    events.push(poll_event);
                }
            }
        }

        Ok(())
    }

    fn drain_notification(&self) {
        drain_notify(self.notify_read);
    }

    fn send_notify(&self) -> io::Result<()> {
        wsa_result(unsafe { wsa::send(self.notify_write, [1u8].as_ptr().cast(), 1, 0) })
    }
}

impl Drop for Poller {
    fn drop(&mut self) {
        unsafe {
            wsa::closesocket(self.notify_read);
            wsa::closesocket(self.notify_write);
        }
    }
}
