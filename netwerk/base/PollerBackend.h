/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_PollerBackend_h
#define mozilla_net_PollerBackend_h

#include <stdint.h>
#include "mozilla/TimeStamp.h"
#include "nscore.h"
#include "nsTArray.h"

namespace mozilla::net {

struct PollerStats;

// A native OS socket/file descriptor: a POSIX fd or a Windows SOCKET, either
// of which fits in an intptr_t. Deliberately not NSPR's PROsfd -- see below.
using PollerFd = intptr_t;

// Interest/readiness bits used by PollerBackend. Deliberately distinct from
// NSPR's PR_POLL_* (prio.h): PollerBackend must not depend on NSPR layered-fd
// semantics at all, since a future backend has NSS push resolved interest
// directly with no NSPR layer walk involved. The one necessary translation
// to/from PR_POLL_* lives at the boundary, in the caller's NSPR-layer-walking
// code (see Poller.h's WalkSocketLayers() / UnmapReadyFlags()), not here.
static constexpr int16_t kPollerRead = 1 << 0;
static constexpr int16_t kPollerWrite = 1 << 1;
static constexpr int16_t kPollerExcept = 1 << 2;  // readiness-only; see below
static constexpr int16_t kPollerError = 1 << 3;
static constexpr int16_t kPollerHangup = 1 << 4;
static constexpr int16_t kPollerInvalid = 1 << 5;

// One ready notification returned by PollerBackend::Wait. |key| is whatever
// opaque pointer the caller supplied to the matching Add()/Modify() call;
// |outFlags| are kPoller* bits describing OS-level readiness only.
// kPollerExcept is readiness-only: it reports an unrequested but real
// out-of-band event the OS surfaced anyway; it is never valid as interest.
struct PollerReadyEvent {
  void* key = nullptr;
  int16_t outFlags = 0;
};

// Abstract interface for OS-level socket readiness polling. Implementations
// own a persistent, stateful registration of native OS file descriptors and
// know nothing about NSPR layered file descriptors or NSS/TLS semantics --
// resolving a socket's TLS-specific interest (walking its NSPR layer stack,
// i.e. ssl_Poll) is the caller's responsibility, with the result fed into
// Modify(). This keeps every concrete backend (poll/select, epoll, kqueue,
// Windows ProcessSocketNotifications) identical in shape regardless of how
// that interest gets resolved -- including a future backend where NSS pushes
// resolved interest directly, removing the per-iteration layer walk.
//
// All methods are socket-thread-only; implementations do no internal
// locking.
class PollerBackend {
 public:
  virtual ~PollerBackend() = default;

  // Registers a native OS fd with the given initial interest (kPollerRead /
  // kPollerWrite, ORed) and an opaque caller-owned key, returned unchanged
  // via PollerReadyEvent::key by Wait(). aFd must not already be registered.
  virtual nsresult Add(PollerFd aFd, int16_t aInterest, void* aKey) = 0;

  // Updates the interest for an already-registered fd. Interest 0 means "not
  // currently interested"; callers should express idleness this way rather
  // than Remove()/re-Add(), so a stateful backend never has to pay kernel
  // registration churn for a socket that is merely quiescent.
  virtual nsresult Modify(PollerFd aFd, int16_t aInterest) = 0;

  // Unregisters a native OS fd. Must be called before the fd is closed.
  virtual nsresult Remove(PollerFd aFd) = 0;

  // Blocks (up to aTimeout) for readiness on any registered fd, or until
  // Wake() is called from another thread. TimeDuration::Forever() waits
  // indefinitely; a zero duration polls without blocking. Returns the number
  // of ready events (>0), 0 on timeout, -1 on error.
  virtual int32_t Wait(const TimeDuration& aTimeout,
                       nsTArray<PollerReadyEvent>& aReady,
                       PollerStats* aStats = nullptr) = 0;

  // Wakes a concurrent (or upcoming) Wait() call from another thread.
  // Implementations may coalesce: multiple calls before the next Wait()
  // returns must cost no more than one wake syscall.
  virtual void Wake() = 0;
};

}  // namespace mozilla::net

#endif  // mozilla_net_PollerBackend_h
