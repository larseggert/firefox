/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! Windows backend using `select()` with a UDP loopback socket for
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
//! Notification uses a self-connected UDP loopback socket bound to
//! `127.0.0.1` on an ephemeral port. This avoids filesystem access and works
//! in sandboxed child processes. See [`create_udp_notify_socket`] for the
//! rationale and alternatives evaluated.

use std::{io, ptr};

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

/// Creates a self-connected UDP loopback socket for cross-thread notification.
///
/// Binds a `SOCK_DGRAM` socket to `127.0.0.1:0`, lets the OS assign an
/// ephemeral port, then `connect()`s the socket to its own address. The
/// resulting non-blocking socket acts as a wake-up channel: `send()` enqueues
/// a datagram that makes the socket readable for `select()`, and
/// `recv()` drains it.
///
/// ## Why not AF_UNIX?
///
/// Every available AF_UNIX mechanism has a blocking issue on Windows:
///
/// - **Filesystem-path sockets**: `bind()` creates an NTFS reparse point,
///   which requires token privileges that Firefox's sandbox (`USER_LIMITED`
///   with `SetLockdownDefaultDacl`) strips. `bind()` returns `WSAEACCES`
///   (10013) before the sandbox's `AllowFileAccess` rules are even consulted,
///   because the denial originates inside the Winsock AF_UNIX provider itself.
///
/// - **Abstract sockets** (null-byte `sun_path` prefix): documented as
///   supported by Microsoft but `connect()` returns `WSAEINVAL` (10022).
///   <https://github.com/microsoft/WSL/issues/4240>
///
/// - **Unnamed sockets**: Windows does not implement `socketpair()` and
///   autobind is unsupported, so there is no way to create a connected pair
///   without a named listener.
///   <https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/>
///
/// - **Named pipes** (`\\.\pipe\...`): not sockets; cannot participate in
///   `select()`.
fn create_udp_notify_socket() -> io::Result<SOCKET> {
    unsafe {
        let sock = wsa::socket(wsa::AF_INET as i32, wsa::SOCK_DGRAM, 0);
        if sock == wsa::INVALID_SOCKET {
            return Err(io::Error::last_os_error());
        }

        let result = (|| -> io::Result<()> {
            let mut addr: wsa::SOCKADDR_IN = std::mem::zeroed();
            addr.sin_family = wsa::AF_INET;
            addr.sin_addr.S_un.S_addr = u32::from_ne_bytes([127, 0, 0, 1]);
            let addr_len = size_of::<wsa::SOCKADDR_IN>() as i32;

            wsa_result(wsa::bind(sock, ptr::from_ref(&addr).cast(), addr_len))?;

            let mut bound_len = addr_len;
            wsa_result(wsa::getsockname(
                sock,
                ptr::from_mut(&mut addr).cast(),
                &mut bound_len,
            ))?;

            wsa_result(wsa::connect(sock, ptr::from_ref(&addr).cast(), addr_len))?;

            let mut nb: u32 = 1;
            wsa_result(wsa::ioctlsocket(sock, wsa::FIONBIO, &mut nb))
        })();

        if let Err(e) = result {
            wsa::closesocket(sock);
            return Err(e);
        }

        Ok(sock)
    }
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
    notify_socket: SOCKET,
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
        let notify_socket = match create_udp_notify_socket() {
            Ok(s) => s,
            Err(e) => {
                eprintln!("necko_poll: create_udp_notify_socket failed: {e}");
                return None;
            }
        };

        Some(Self {
            fd_state: FxHashMap::with_capacity_and_hasher(32, FxBuildHasher),
            notify_socket,
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

        self.read_set.add(self.notify_socket);

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
        drain_notify(self.notify_socket);
    }

    fn send_notify(&self) -> io::Result<()> {
        wsa_result(unsafe { wsa::send(self.notify_socket, [1u8].as_ptr().cast(), 1, 0) })
    }
}

impl Drop for Poller {
    fn drop(&mut self) {
        unsafe {
            wsa::closesocket(self.notify_socket);
        }
    }
}
