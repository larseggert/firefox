/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_Poller_h
#define mozilla_net_Poller_h

#include <stdint.h>
#include "PollerBackend.h"
#include "nsTHashMap.h"
#include "prio.h"

namespace mozilla::net {

// Fixed log2-bucketed histogram.  Bucket 0 = zero-valued samples;
// bucket k >= 1 covers [2^(k-1), 2^k - 1]; bucket 32 overflows >= 2^31.
struct Histogram {
  uint64_t mBuckets[33] = {};
  uint64_t mCount = 0;
  uint64_t mMax = 0;

  void Add(uint64_t aValue);
  uint64_t Percentile(double aFraction) const;
  bool Empty() const { return mCount == 0; }
};

// Per-backend polling stats, one instance per nsSocketTransportService.
// Printed to stderr at shutdown.
struct PollerStats {
  uint64_t iterations = 0;
  uint64_t shortCircuits = 0;  // skipped the kernel call
  uint64_t eintrRetries = 0;   // Unix only

  Histogram fdCount;       // registered fd count per iteration
  Histogram readyCount;    // ready descriptor count per iteration
  Histogram layerWalkUs;   // time spent in per-socket NSPR layer walk
  Histogram kernelWaitUs;  // time spent in the backend's kernel wait call

  // Populated only by stateful backends (epoll/kqueue/Windows); the portable
  // backend leaves them empty.
  Histogram addFdUs;
  Histogram removeFdUs;
  Histogram modifyFdUs;
  Histogram syscallsPerIteration;
  Histogram syscallTimePerIterUs;

  void Print(const char* aLabel) const;
};

// Result of walking one socket's NSPR I/O-layer stack (WalkSocketLayers())
// to resolve its OS-level interest for this iteration.
struct LayerWalkResult {
  // True if a layer (e.g. NSS's ssl_Poll reporting buffered decrypted data,
  // or an already-invalid fd) is already ready with no OS-level event
  // needed. When true, |outFlags| is valid and the caller should dispatch
  // immediately without registering interest with a PollerBackend this
  // iteration; when false, |osInterest| and |directionMap| are valid
  // instead.
  bool shortCircuited = false;

  // Valid iff shortCircuited: PR_POLL_* bits the socket is ready for, to
  // hand directly to nsASocketHandler::OnSocketReady.
  int16_t outFlags = 0;

  // Valid iff !shortCircuited: kPoller{Read,Write} bits to register with a
  // PollerBackend via Add()/Modify().
  int16_t osInterest = 0;

  // Valid iff !shortCircuited: direction-mapping bits recording which
  // PR_POLL_READ/WRITE request (aWantFlags) each OS-level interest bit
  // corresponds to -- an NSPR layer (e.g. mid-TLS-handshake) may need the
  // opposite OS direction from what the caller asked for. Feed the OS
  // readiness this fd eventually reports, plus this direction map, into
  // UnmapReadyFlags() to recover the PR_POLL_* flags the caller actually
  // asked about.
  int16_t directionMap = 0;
};

// Walks aFd's NSPR I/O-layer stack (calling PRIOMethods::poll, e.g. NSS's
// ssl_Poll) to resolve aWantFlags (PR_POLL_READ | PR_POLL_WRITE) into either
// an immediate short-circuit result or an OS-level interest to poll for.
// This is the per-iteration cost a future NSS readiness-push API removes:
// see the design notes above nsSocketTransportService::DoPollIterationWithBackend.
// Ported from _pr_poll_with_poll's per-descriptor loop
// (nsprpub/pr/src/pthreads/ptio.c); see PR_Poll for the full contract.
LayerWalkResult WalkSocketLayers(PRFileDesc* aFd, int16_t aWantFlags);

// Direction-mapping bits recording which PR_POLL_* direction(s) a
// kPollerRead/Write interest bit was requested on behalf of. Match
// _PR_POLL_READ_SYS_READ etc. in NSPR's private primpl.h --
// WalkSocketLayers()/UnmapReadyFlags() are a from-scratch reimplementation
// of that same read<->write direction-flip bookkeeping, not a port of
// NSPR's internal representation. Exposed here (not file-local to
// Poller.cpp) so a caller with an equivalent mapping from a different
// source -- e.g. nsSocketTransportService::OnTLSReadinessChanged(), packing
// NSS's SSLReadiness booleans -- can still call UnmapReadyFlags() directly
// instead of duplicating its logic.
constexpr int16_t kPollReadSysRead = 0x1;
constexpr int16_t kPollReadSysWrite = 0x2;
constexpr int16_t kPollWriteSysRead = 0x4;
constexpr int16_t kPollWriteSysWrite = 0x8;

// Un-maps aOsReadyFlags (kPoller* bits, as returned by a PollerBackend for
// one fd) back to the PR_POLL_* flags the caller actually asked about, using
// the direction map a WalkSocketLayers() call produced this same
// iteration for the same fd.
int16_t UnmapReadyFlags(int16_t aOsReadyFlags, int16_t aDirectionMap);

// Portable PollerBackend built on poll(2) / WSAPoll. Correct on every
// platform; the backend used everywhere until a per-platform backend
// (epoll/kqueue/Windows notifications) is available and enabled.
class PollPollerBackend final : public PollerBackend {
 public:
  PollPollerBackend() = default;
  ~PollPollerBackend() = default;

  nsresult Add(PollerFd aFd, int16_t aInterest, void* aKey) override;
  nsresult Modify(PollerFd aFd, int16_t aInterest) override;
  nsresult Remove(PollerFd aFd) override;
  int32_t Wait(const TimeDuration& aTimeout,
               nsTArray<PollerReadyEvent>& aReady,
               PollerStats* aStats = nullptr) override;
  void Wake() override;

 private:
  struct Registration {
    int16_t mInterest = 0;
    void* mKey = nullptr;
  };

  // Keyed by the native fd itself -- already the only identifier
  // Add()/Modify()/Remove()'s caller (nsSocketTransportService2.cpp) ever
  // has, and already guaranteed unique while registered (see Add()'s
  // MOZ_RELEASE_ASSERT below). Keying the registration store by it
  // directly gives Modify()/Remove() O(1) lookup without a separate
  // fd->index side table, which an nsTArray<Registration> would need and
  // which UnorderedRemoveElementAt()'s last-element swap would otherwise
  // silently invalidate for whichever entry got swapped.
  nsTHashMap<intptr_t, Registration> mRegistrations;
};

}  // namespace mozilla::net

#endif  // mozilla_net_Poller_h
