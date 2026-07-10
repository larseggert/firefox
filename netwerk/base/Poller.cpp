/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Poller.h"

#include <chrono>
#include <stdio.h>
#include <thread>
#include "mozilla/IntegerPrintfMacros.h"
#include "private/pprio.h"

#ifdef XP_WIN
#  include <winsock2.h>
using PollFd = WSAPOLLFD;
#else
#  include <errno.h>
#  include <poll.h>
using PollFd = struct pollfd;
#endif

namespace mozilla::net {

// Scratch bits recording which PR_POLL_* direction(s) a kPollerRead/Write
// interest bit was requested on behalf of. Match _PR_POLL_READ_SYS_READ etc.
// in NSPR's private primpl.h -- WalkSocketLayers()/UnmapReadyFlags() are a
// from-scratch reimplementation of that same read<->write direction-flip
// bookkeeping, not a port of NSPR's internal representation.
static constexpr int16_t kPollReadSysRead = 0x1;
static constexpr int16_t kPollReadSysWrite = 0x2;
static constexpr int16_t kPollWriteSysRead = 0x4;
static constexpr int16_t kPollWriteSysWrite = 0x8;

void Histogram::Add(uint64_t aValue) {
  ++mCount;
  if (aValue > mMax) mMax = aValue;
  // Bucket 0 = value 0; bucket k = values in [2^(k-1), 2^k - 1], k >= 1.
  uint32_t bucket = 0;
  for (uint64_t v = aValue; v > 0; v >>= 1) ++bucket;
  if (bucket > 32) bucket = 32;
  ++mBuckets[bucket];
}

uint64_t Histogram::Percentile(double aFraction) const {
  if (mCount == 0) return 0;
  uint64_t target = static_cast<uint64_t>(aFraction * mCount);
  uint64_t accumulated = 0;
  for (int i = 0; i <= 32; ++i) {
    accumulated += mBuckets[i];
    if (accumulated > target) return i == 0 ? 0 : (1ULL << (i - 1));
  }
  return mMax;
}

void PollerStats::Print(const char* aLabel) const {
  if (iterations == 0) return;
  printf("PollerStats[%s]: %" PRIu64 " iterations, %" PRIu64
         " short-circuits, %" PRIu64 " EINTR retries\n",
         aLabel, iterations, shortCircuits, eintrRetries);
  auto printHist = [](const Histogram& h, const char* aName,
                      const char* aUnit) {
    if (h.Empty()) {
      printf("  %-26s (empty)\n", aName);
      return;
    }
    printf("  %-26s p50=%" PRIu64 "%s p95=%" PRIu64 "%s p99=%" PRIu64
           "%s max=%" PRIu64 "%s\n",
           aName, h.Percentile(0.50), aUnit, h.Percentile(0.95), aUnit,
           h.Percentile(0.99), aUnit, h.mMax, aUnit);
  };
  printHist(fdCount, "fd_count:", "");
  printHist(readyCount, "ready_count:", "");
  printHist(layerWalkUs, "layer_walk:", "us");
  printHist(kernelWaitUs, "kernel_wait:", "us");
  printHist(addFdUs, "add_fd:", "us");
  printHist(removeFdUs, "remove_fd:", "us");
  printHist(modifyFdUs, "modify_fd:", "us");
  printHist(syscallsPerIteration, "syscalls_per_iter:", "");
  printHist(syscallTimePerIterUs, "syscall_time/iter:", "us");
}

// Ported from the per-descriptor loop body of _pr_poll_with_poll
// (nsprpub/pr/src/pthreads/ptio.c); see PR_Poll for the full layered-poll
// contract this reproduces for a single descriptor.
LayerWalkResult WalkSocketLayers(PRFileDesc* aFd, int16_t aWantFlags) {
  LayerWalkResult r;
  if (!aFd || !aWantFlags) {
    return r;
  }

  int16_t inR = 0, inW = 0, outR = 0, outW = 0;
  if (aWantFlags & PR_POLL_READ) {
    inR = aFd->methods->poll(aFd, aWantFlags & ~PR_POLL_WRITE, &outR);
  }
  if (aWantFlags & PR_POLL_WRITE) {
    inW = aFd->methods->poll(aFd, aWantFlags & ~PR_POLL_READ, &outW);
  }

  if ((inR & outR) || (inW & outW)) {
    r.shortCircuited = true;
    r.outFlags = outR | outW;
    return r;
  }

  PROsfd osfd = PR_FileDesc2NativeHandle(aFd);
  bool fdInvalid =
#ifdef XP_WIN
      (osfd == static_cast<PROsfd>(INVALID_SOCKET));
#else
      (osfd < 0);
#endif
  if (fdInvalid) {
    r.shortCircuited = true;
    r.outFlags = PR_POLL_NVAL;
    return r;
  }

  // Do NOT request kPollerExcept even when aWantFlags has PR_POLL_EXCEPT: on
  // macOS, requesting POLLPRI causes poll() to echo it in revents whenever
  // the fd has data, mapping back to PR_POLL_EXCEPT and triggering a
  // spurious PollableEvent repair loop. True OOB (rare; Firefox does not use
  // it) still surfaces as an unrequested kPollerExcept and is passed through
  // by PollerBackend implementations regardless.
  if (inR & PR_POLL_READ) {
    r.scratch |= kPollReadSysRead;
    r.osInterest |= kPollerRead;
  }
  if (inR & PR_POLL_WRITE) {
    r.scratch |= kPollReadSysWrite;
    r.osInterest |= kPollerWrite;
  }
  if (inW & PR_POLL_READ) {
    r.scratch |= kPollWriteSysRead;
    r.osInterest |= kPollerRead;
  }
  if (inW & PR_POLL_WRITE) {
    r.scratch |= kPollWriteSysWrite;
    r.osInterest |= kPollerWrite;
  }
  return r;
}

int16_t UnmapReadyFlags(int16_t aOsReadyFlags, int16_t aScratch) {
  int16_t outFlags = 0;
  if (aOsReadyFlags & kPollerRead) {
    if (aScratch & kPollReadSysRead) outFlags |= PR_POLL_READ;
    if (aScratch & kPollWriteSysRead) outFlags |= PR_POLL_WRITE;
  }
  if (aOsReadyFlags & kPollerWrite) {
    if (aScratch & kPollReadSysWrite) outFlags |= PR_POLL_READ;
    if (aScratch & kPollWriteSysWrite) outFlags |= PR_POLL_WRITE;
  }
  // Except/error/hangup/invalid are not subject to direction remapping --
  // they describe the fd itself, not a read or write request on it.
  if (aOsReadyFlags & kPollerExcept) outFlags |= PR_POLL_EXCEPT;
  if (aOsReadyFlags & kPollerError) outFlags |= PR_POLL_ERR;
  if (aOsReadyFlags & kPollerHangup) outFlags |= PR_POLL_HUP;
  if (aOsReadyFlags & kPollerInvalid) outFlags |= PR_POLL_NVAL;
  return outFlags;
}

static constexpr uint32_t kStackPollCount = 64;

// PollFd buffer: stack-resident for small counts, heap for larger. Every
// slot is filled from a real registration before the kernel call -- unlike
// NSPR's PR_Poll, PollPollerBackend never has to leave "skip this one"
// holes in the array, since short-circuited fds are resolved by the caller
// (see WalkSocketLayers()) before Wait() is ever invoked.
struct PollBuf {
  PollFd stack[kStackPollCount];
  PollFd* data;

  explicit PollBuf(uint32_t aCount)
      : data(aCount <= kStackPollCount ? stack : new PollFd[aCount]) {}
  ~PollBuf() {
    if (data != stack) delete[] data;
  }
  PollBuf(const PollBuf&) = delete;
  PollBuf& operator=(const PollBuf&) = delete;
};

nsresult PollPollerBackend::Add(PollerFd aFd, int16_t aInterest, void* aKey) {
  // Enforced in all builds, not just debug: a double-Add() would otherwise
  // silently clobber the existing registration, which Remove() could then
  // only ever unregister once (leaking the other logical registration's
  // interest forever).
  MOZ_RELEASE_ASSERT(!mRegistrations.Contains(aFd), "fd already registered");
  mRegistrations.InsertOrUpdate(aFd, Registration{aInterest, aKey});
  return NS_OK;
}

nsresult PollPollerBackend::Modify(PollerFd aFd, int16_t aInterest) {
  auto entry = mRegistrations.Lookup(aFd);
  if (!entry) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  entry.Data().mInterest = aInterest;
  return NS_OK;
}

nsresult PollPollerBackend::Remove(PollerFd aFd) {
  if (!mRegistrations.Remove(aFd)) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  return NS_OK;
}

void PollPollerBackend::Wake() {
  // No-op: Wait() blocks in poll()/WSAPoll on a set that includes the
  // caller's wakeup fd (registered like any other via Add()); writing to
  // that fd is what actually wakes it, and the caller already does that
  // itself. This hook exists for backends whose blocking primitive is not
  // itself an fd in the polled set.
}

int32_t PollPollerBackend::Wait(const TimeDuration& aTimeout,
                                nsTArray<PollerReadyEvent>& aReady,
                                PollerStats* aStats) {
  aReady.Clear();

  bool isForever = aTimeout == TimeDuration::Forever();
  int msecs = isForever ? -1 : static_cast<int>(aTimeout.ToMilliseconds());
#ifndef XP_WIN
  // Only the poll(2) EINTR-retry loop below reads this; WSAPoll has no
  // retry loop, so this would be an unused variable (-Werror) on Windows.
  TimeStamp start = isForever ? TimeStamp() : TimeStamp::Now();
#endif

  // Callers (see nsSocketTransportService::AttachSocket()/MoveToIdleList())
  // register a socket once for its whole attached lifetime and report
  // idleness as zero interest rather than unregistering, so a stateful
  // kernel backend (epoll/kqueue) never pays add/remove churn for a merely
  // quiescent socket. poll(2)/WSAPoll are O(array size) regardless of
  // interest, though, so unlike a stateful backend this one must still
  // filter zero-interest registrations out of its own marshalled array to
  // avoid paying O(total attached) instead of O(active) per call. PollBuf is
  // sized to the worst case (all attached sockets active) so this filtering
  // and the fill below are a single pass over mRegistrations; activeKeys
  // records each filled pollBuf slot's caller-supplied key directly (no
  // index to map back through -- mRegistrations is keyed by fd, not by
  // position).
  PollBuf pb(mRegistrations.Count());
  PollFd* pollBuf = pb.data;
  nsTArray<void*> activeKeys;
  activeKeys.SetCapacity(mRegistrations.Count());
  uint32_t count = 0;
  for (auto iter = mRegistrations.ConstIter(); !iter.Done(); iter.Next()) {
    const Registration& reg = iter.Data();
    if (reg.mInterest == 0) continue;
    pollBuf[count].fd = static_cast<decltype(pollBuf[count].fd)>(iter.Key());
    pollBuf[count].events = 0;
    pollBuf[count].revents = 0;
    if (reg.mInterest & kPollerRead) pollBuf[count].events |= POLLIN;
    if (reg.mInterest & kPollerWrite) pollBuf[count].events |= POLLOUT;
    activeKeys.AppendElement(reg.mKey);
    ++count;
  }

  if (count == 0) {
    // Should not happen in practice -- the caller keeps at least a wakeup fd
    // registered -- but approximate PR_Poll's "nothing to poll, wait out the
    // timeout" behavior rather than returning immediately. An unbounded
    // (Forever()) timeout is clamped to a bounded busy-wait, mirroring the
    // legacy PR_Poll path's explicit 25ms fallback for the same "no pollable
    // event" degraded state: returning immediately here would otherwise spin
    // the socket thread at 100% CPU instead of blocking.
    if (aStats) ++aStats->iterations;
    constexpr auto kBusyWaitFallback = std::chrono::milliseconds(25);
    std::this_thread::sleep_for(
        isForever ? kBusyWaitFallback
                  : std::chrono::milliseconds(std::max(msecs, 0)));
    return 0;
  }

  if (aStats) aStats->fdCount.Add(count);

  TimeStamp kernelStart = aStats ? TimeStamp::Now() : TimeStamp();
  int32_t ready;
#ifdef XP_WIN
  ready = WSAPoll(pollBuf, static_cast<ULONG>(count), msecs);
  if (ready == SOCKET_ERROR) {
    fprintf(stderr, "PollPollerBackend::Wait: WSAPoll failed, err=%d\n",
            WSAGetLastError());
    ready = -1;
  }
#else
  for (;;) {
    ready = poll(pollBuf, static_cast<nfds_t>(count), msecs);
    if (ready != -1 || errno != EINTR) {
      if (ready == -1) {
        fprintf(stderr, "PollPollerBackend::Wait: poll failed, errno=%d\n",
                errno);
      }
      break;
    }
    if (aStats) ++aStats->eintrRetries;
    if (isForever) continue;
    if (msecs == 0) {
      ready = 0;
      break;
    }
    double elapsedMs = (TimeStamp::Now() - start).ToMilliseconds();
    double totalMs = aTimeout.ToMilliseconds();
    if (elapsedMs >= totalMs) {
      ready = 0;
      break;
    }
    msecs = static_cast<int>(totalMs - elapsedMs);
  }
#endif

  if (aStats) {
    aStats->kernelWaitUs.Add(
        static_cast<uint64_t>((TimeStamp::Now() - kernelStart).ToMicroseconds()));
  }

  if (ready > 0) {
    for (uint32_t i = 0; i < count; ++i) {
      int16_t rev = pollBuf[i].revents;
      if (!rev) continue;
      int16_t outFlags = 0;
      if (rev & POLLIN) outFlags |= kPollerRead;
      if (rev & POLLOUT) outFlags |= kPollerWrite;
#ifndef XP_WIN
      if (rev & POLLPRI) outFlags |= kPollerExcept;
#endif
      if (rev & POLLERR) {
        outFlags |= kPollerError;
#ifdef XP_WIN
        // NSPR's PR_Poll on Windows (w32poll.c) is select()-based and never
        // produces PR_POLL_ERR at all -- every socket error there, including
        // a failed non-blocking connect, surfaces exclusively via the
        // exceptfds set, mapped to PR_POLL_EXCEPT. SocketConnectContinue()
        // (prsocket.c) specifically gates its getsockopt(SO_ERROR)/
        // connect-error-translation path on PR_POLL_EXCEPT being set, with
        // no equivalent check for PR_POLL_ERR/PR_POLL_HUP -- without this, a
        // failed connect on Windows (WSAPoll documents POLLHUP | POLLERR |
        // POLLWRNORM for that case) gets misread as succeeded, since
        // PR_POLL_WRITE is set too. Add kPollerExcept so it reaches that
        // check, matching legacy behavior.
        outFlags |= kPollerExcept;
#endif
      }
      if (rev & POLLHUP) {
        outFlags |= kPollerHangup;
#ifdef XP_WIN
        // Per Microsoft's WSAPoll documentation, a full peer-side close is
        // reported as POLLHUP alone (a TCP FIN/half-close is POLLRDNORM
        // instead). NSPR's PR_Poll on Windows (w32poll.c) is select()-based
        // and has no HUP concept at all -- there, any peer disconnect only
        // ever shows up as read-ready. Add kPollerRead too so a socket
        // registered for read (or write-flipped-to-read, via the
        // scratch-bit direction-inversion in UnmapReadyFlags) still gets
        // woken on a full close, matching that behavior.
        outFlags |= kPollerRead;
#endif
      }
      if (rev & POLLNVAL) outFlags |= kPollerInvalid;
      if (outFlags) {
        aReady.AppendElement(PollerReadyEvent{activeKeys[i], outFlags});
      }
    }
  }

  if (aStats) {
    ++aStats->iterations;
    aStats->readyCount.Add(ready > 0 ? static_cast<uint32_t>(ready) : 0);
  }
  return ready;
}

}  // namespace mozilla::net
