/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::{
    collections::VecDeque,
    io,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
    num::NonZeroUsize,
};

use firefox_on_glean::{
    metrics::webrtc_udp,
    private::{LocalCustomDistribution, LocalMemoryDistribution},
};
use neqo_common::{datagram, Tos};
use neqo_udp::RecvBuf;

const R_OK: i32 = 0;
const R_NEEDS_FLUSH_POST: i32 = 1;
const R_WOULDBLOCK: i32 = 8;
const R_IO_ERROR: i32 = 13;

const AF_INET: u16 = 2;
#[cfg(not(target_os = "windows"))]
const AF_INET6: u16 = {
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        10
    }
    #[cfg(any(target_os = "macos", target_os = "ios", target_os = "freebsd"))]
    {
        30
    }
};
#[cfg(target_os = "windows")]
const AF_INET6: u16 = 23;

#[cfg(unix)]
type BorrowedSocket = std::os::fd::BorrowedFd<'static>;
#[cfg(windows)]
type BorrowedSocket = std::os::windows::io::BorrowedSocket<'static>;

struct BufferedDatagram {
    data: Vec<u8>,
    source: SocketAddr,
}

/// Opaque handle to a neqo-udp backed UDP socket.
pub struct WebrtcUdpSocket {
    socket: neqo_udp::Socket<BorrowedSocket>,
    recv_buf: RecvBuf,
    buffered: VecDeque<BufferedDatagram>,
    local_addr: SocketAddr,
    pending_batch: Option<datagram::Batch>,
    flush_posted: bool,

    segment_size_sent: LocalMemoryDistribution<'static>,
    segment_size_received: LocalMemoryDistribution<'static>,
    size_sent: LocalMemoryDistribution<'static>,
    size_received: LocalMemoryDistribution<'static>,
    segments_sent: LocalCustomDistribution<'static>,
    segments_received: LocalCustomDistribution<'static>,
}

fn addr_from_raw(family: u16, addr_bytes: *const u8, port: u16) -> Option<SocketAddr> {
    if addr_bytes.is_null() {
        return None;
    }
    match family {
        AF_INET => {
            let octets: [u8; 4] = unsafe { std::ptr::read(addr_bytes.cast()) };
            Some(SocketAddr::new(IpAddr::V4(Ipv4Addr::from(octets)), port))
        }
        AF_INET6 => {
            let octets: [u8; 16] = unsafe { std::ptr::read(addr_bytes.cast()) };
            Some(SocketAddr::new(IpAddr::V6(Ipv6Addr::from(octets)), port))
        }
        _ => None,
    }
}

fn addr_to_raw(addr: &SocketAddr, out_family: &mut u16, out_addr: *mut u8, out_port: &mut u16) {
    *out_port = addr.port();
    match addr {
        SocketAddr::V4(v4) => {
            *out_family = AF_INET;
            unsafe {
                std::ptr::copy_nonoverlapping(v4.ip().octets().as_ptr(), out_addr, 4);
            }
        }
        SocketAddr::V6(v6) => {
            *out_family = AF_INET6;
            unsafe {
                std::ptr::copy_nonoverlapping(v6.ip().octets().as_ptr(), out_addr, 16);
            }
        }
    }
}

impl WebrtcUdpSocket {
    fn flush_pending_batch(&mut self) -> io::Result<()> {
        self.flush_posted = false;
        if let Some(batch) = self.pending_batch.take() {
            self.socket.send(&batch)?;

            self.size_sent.accumulate(batch.data().len() as u64);
            self.segments_sent.accumulate(batch.num_datagrams() as u64);
            for _ in 0..(batch.data().len() / batch.datagram_size()) {
                self.segment_size_sent
                    .accumulate(batch.datagram_size().get() as u64);
            }
            self.segment_size_sent.accumulate(
                batch
                    .data()
                    .len()
                    .checked_rem(batch.datagram_size().get())
                    .expect("datagram_size is a NonZeroUsize") as u64,
            );
        }
        Ok(())
    }
}

/// Create a new `WebrtcUdpSocket` wrapping the given raw file descriptor.
///
/// The fd is borrowed (not owned); the caller retains ownership and must
/// ensure it outlives the returned socket. Returns null on failure.
///
/// # Safety
///
/// `fd` must be a valid open socket file descriptor.
#[no_mangle]
pub unsafe extern "C" fn webrtc_udp_socket_new(
    fd: i64,
    local_family: u16,
    local_addr_bytes: *const u8,
    local_port: u16,
) -> *mut WebrtcUdpSocket {
    let local_addr = match addr_from_raw(local_family, local_addr_bytes, local_port) {
        Some(a) => a,
        None => {
            log::error!("webrtc_udp_socket_new: invalid local address");
            return std::ptr::null_mut();
        }
    };

    #[cfg(unix)]
    let borrowed = {
        use std::os::fd::{BorrowedFd, RawFd};
        if fd == -1 {
            log::error!("webrtc_udp_socket_new: invalid fd {fd}");
            return std::ptr::null_mut();
        }
        let Ok(raw) = RawFd::try_from(fd) else {
            log::error!("webrtc_udp_socket_new: fd {fd} out of range for RawFd");
            return std::ptr::null_mut();
        };
        BorrowedFd::borrow_raw(raw)
    };

    #[cfg(windows)]
    let borrowed = {
        use std::os::windows::io::{BorrowedSocket, RawSocket};
        if fd as usize == winapi::um::winsock2::INVALID_SOCKET {
            log::error!("webrtc_udp_socket_new: invalid socket {fd}");
            return std::ptr::null_mut();
        }
        let Ok(raw) = RawSocket::try_from(fd) else {
            log::error!("webrtc_udp_socket_new: socket {fd} out of range");
            return std::ptr::null_mut();
        };
        BorrowedSocket::borrow_raw(raw)
    };

    let socket = match neqo_udp::Socket::new(borrowed) {
        Ok(s) => s,
        Err(e) => {
            log::error!("webrtc_udp_socket_new: neqo_udp::Socket::new failed: {e}");
            return std::ptr::null_mut();
        }
    };

    Box::into_raw(Box::new(WebrtcUdpSocket {
        socket,
        recv_buf: RecvBuf::default(),
        buffered: VecDeque::new(),
        local_addr,
        pending_batch: None,
        flush_posted: false,
        segment_size_sent: webrtc_udp::segment_size_sent.start_buffer(),
        segment_size_received: webrtc_udp::segment_size_received.start_buffer(),
        size_sent: webrtc_udp::size_sent.start_buffer(),
        size_received: webrtc_udp::size_received.start_buffer(),
        segments_sent: webrtc_udp::segments_sent.start_buffer(),
        segments_received: webrtc_udp::segments_received.start_buffer(),
    }))
}

/// Destroy a `WebrtcUdpSocket`. Does NOT close the underlying fd.
///
/// # Safety
///
/// `socket` must be a valid pointer returned by `webrtc_udp_socket_new`,
/// or null (in which case this is a no-op).
#[no_mangle]
pub unsafe extern "C" fn webrtc_udp_socket_destroy(socket: *mut WebrtcUdpSocket) {
    if !socket.is_null() {
        let mut s = Box::from_raw(socket);
        let _ = s.flush_pending_batch();
        drop(s);
    }
}

/// Receive a single datagram from the socket.
///
/// On the first call (or when the internal buffer is empty), performs a
/// batched kernel recv and buffers all received datagrams. Subsequent calls
/// return buffered datagrams without a syscall.
///
/// Returns: 0 on success, 8 (`R_WOULDBLOCK`) when no data available,
/// 13 (`R_IO_ERROR`) on error.
///
/// # Safety
///
/// All pointer arguments must be valid and writable. `buf` must point to
/// at least `buf_max` bytes. `from_addr` must point to at least 16 bytes.
// TODO: Avoid per-datagram Vec allocation by keeping the RecvBuf data alive
// and returning slices into it. Requires tracking RecvBuf lifetime across calls.
// TODO: Expose ECN codepoints from received datagrams to the WebRTC congestion
// controller, enabling L4S/ECN-based congestion feedback.
#[no_mangle]
pub unsafe extern "C" fn webrtc_udp_recv(
    socket: *mut WebrtcUdpSocket,
    buf: *mut u8,
    buf_max: usize,
    len_out: *mut usize,
    from_family: *mut u16,
    from_addr: *mut u8,
    from_port: *mut u16,
) -> i32 {
    let socket = &mut *socket;

    if let Err(e) = socket.flush_pending_batch() {
        log::info!("webrtc_udp_recv: flush error: {e}");
        return R_IO_ERROR;
    }

    if socket.buffered.is_empty() {
        match socket.socket.recv(socket.local_addr, &mut socket.recv_buf) {
            Ok(datagrams) => {
                let mut total_size = 0usize;
                let mut segment_count = 0u64;
                for dg in datagrams {
                    let data = dg.as_ref().to_vec();
                    socket.segment_size_received.accumulate(data.len() as u64);
                    total_size += data.len();
                    segment_count += 1;
                    socket.buffered.push_back(BufferedDatagram {
                        data,
                        source: dg.source(),
                    });
                }
                socket.size_received.accumulate(total_size as u64);
                socket.segments_received.accumulate(segment_count);
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                return R_WOULDBLOCK;
            }
            Err(e) => {
                log::info!("webrtc_udp_recv: recv error: {e}");
                return R_IO_ERROR;
            }
        }
    }

    let Some(pkt) = socket.buffered.pop_front() else {
        return R_WOULDBLOCK;
    };

    let copy_len = pkt.data.len().min(buf_max);
    std::ptr::copy_nonoverlapping(pkt.data.as_ptr(), buf, copy_len);
    *len_out = copy_len;
    addr_to_raw(&pkt.source, &mut *from_family, from_addr, &mut *from_port);

    R_OK
}

/// Send a single datagram through the socket.
///
/// Returns: 0 on success, 8 (`R_WOULDBLOCK`) if the send would block,
/// 13 (`R_IO_ERROR`) on error.
///
/// # Safety
///
/// `buf` must point to at least `len` bytes. `addr_bytes` must point to
/// 4 bytes (IPv4) or 16 bytes (IPv6) depending on `addr_family`.
#[no_mangle]
pub unsafe extern "C" fn webrtc_udp_send(
    socket: *mut WebrtcUdpSocket,
    buf: *const u8,
    len: usize,
    addr_family: u16,
    addr_bytes: *const u8,
    port: u16,
) -> i32 {
    let socket = &mut *socket;

    let dst = match addr_from_raw(addr_family, addr_bytes, port) {
        Some(a) => a,
        None => {
            log::error!("webrtc_udp_send: invalid destination address");
            return R_IO_ERROR;
        }
    };

    let data = std::slice::from_raw_parts(buf, len);

    match socket.socket.send_buffer(dst, Tos::default(), data, None) {
        Ok(()) => R_OK,
        Err(e) if e.kind() == io::ErrorKind::WouldBlock => R_WOULDBLOCK,
        Err(e) => {
            log::info!("webrtc_udp_send: send error: {e}");
            R_IO_ERROR
        }
    }
}

/// Add a datagram to the pending GSO batch.
///
/// Datagrams in a batch share the same destination and segment size.
/// When the destination or segment size changes, or the batch is full
/// (`max_gso_segments` reached), the pending batch is automatically flushed
/// before starting a new one.
///
/// Returns:
/// - 0 (`R_OK`) if the datagram was added and a flush is already posted.
/// - 1 (`R_NEEDS_FLUSH_POST`) if the datagram was added and the caller
///   should post a deferred flush (e.g. via `NS_DispatchToCurrentThread`)
///   that calls `webrtc_udp_send_batch_flush`. This groups all sends within
///   one event loop turn into a single GSO syscall.
/// - 8 (`R_WOULDBLOCK`) / 13 (`R_IO_ERROR`) on failure.
///
/// # Flush strategy
///
/// The deferred-runnable approach batches all sends within one event loop
/// turn with minimal code changes, but adds up to one event loop turn of
/// latency for the last batch. Alternative strategies to consider:
/// - Flush from the WebRTC pacer when a pacing round completes, giving
///   precise burst boundaries but requiring libwebrtc changes.
/// - Flush in the socket poll callback before entering the OS poll,
///   avoiding the runnable overhead but requiring nsSocketTransportService
///   integration.
/// - Use a short timer (e.g. 1ms) to cap flush latency independently of
///   the event loop, at the cost of additional timer management.
///
/// # Safety
///
/// `buf` must point to at least `len` bytes. `addr_bytes` must point to
/// 4 bytes (IPv4) or 16 bytes (IPv6) depending on `addr_family`.
#[no_mangle]
pub unsafe extern "C" fn webrtc_udp_send_batch_add(
    socket: *mut WebrtcUdpSocket,
    buf: *const u8,
    len: usize,
    addr_family: u16,
    addr_bytes: *const u8,
    port: u16,
) -> i32 {
    let socket = &mut *socket;

    let dst = match addr_from_raw(addr_family, addr_bytes, port) {
        Some(a) => a,
        None => {
            log::error!("webrtc_udp_send_batch_add: invalid destination address");
            return R_IO_ERROR;
        }
    };

    let data = std::slice::from_raw_parts(buf, len);
    let max_segments = socket.socket.max_gso_segments();

    let needs_flush = socket.pending_batch.as_ref().is_some_and(|batch| {
        batch.destination() != dst
            || batch.datagram_size().get() != len
            || batch.num_datagrams() >= max_segments
    });

    if needs_flush {
        if let Err(e) = socket.flush_pending_batch() {
            if e.kind() == io::ErrorKind::WouldBlock {
                return R_WOULDBLOCK;
            }
            log::info!("webrtc_udp_send_batch_add: flush error: {e}");
            return R_IO_ERROR;
        }
    }

    match &mut socket.pending_batch {
        Some(batch) => {
            if !batch.push_datagram(data) {
                log::error!("webrtc_udp_send_batch_add: push_datagram rejected");
                return R_IO_ERROR;
            }
        }
        None => {
            let Some(segment_size) = NonZeroUsize::new(len) else {
                log::error!("webrtc_udp_send_batch_add: zero-length datagram");
                return R_IO_ERROR;
            };
            socket.pending_batch = Some(datagram::Batch::new(
                socket.local_addr,
                dst,
                Tos::default(),
                segment_size,
                data.to_vec(),
            ));
        }
    }

    if socket.flush_posted {
        R_OK
    } else {
        socket.flush_posted = true;
        R_NEEDS_FLUSH_POST
    }
}

/// Flush the pending GSO batch, sending all accumulated datagrams.
///
/// No-op if the batch is empty.
///
/// Returns: 0 on success, 8 (`R_WOULDBLOCK`) if the send would block,
/// 13 (`R_IO_ERROR`) on error.
///
/// # Safety
///
/// `socket` must be a valid pointer returned by `webrtc_udp_socket_new`.
#[no_mangle]
pub unsafe extern "C" fn webrtc_udp_send_batch_flush(socket: *mut WebrtcUdpSocket) -> i32 {
    let socket = &mut *socket;

    match socket.flush_pending_batch() {
        Ok(()) => R_OK,
        Err(e) if e.kind() == io::ErrorKind::WouldBlock => R_WOULDBLOCK,
        Err(e) => {
            log::info!("webrtc_udp_send_batch_flush: flush error: {e}");
            R_IO_ERROR
        }
    }
}

/// Returns the maximum number of GSO segments supported by this socket.
///
/// # Safety
///
/// `socket` must be a valid pointer returned by `webrtc_udp_socket_new`.
#[no_mangle]
pub unsafe extern "C" fn webrtc_udp_max_gso_segments(socket: *mut WebrtcUdpSocket) -> usize {
    let socket = &*socket;
    socket.socket.max_gso_segments()
}
