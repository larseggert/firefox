/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Differential tests for mozilla::net::WalkSocketLayers()/PollPollerBackend
// vs PR_Poll.  Each test prepares two independent fd sets in the same state,
// runs PR_Poll on one and the new composition on the other, and asserts the
// per-descriptor out_flags agree.

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <string.h>
#include <thread>
#include <vector>
#include "Poller.h"
#include "mozilla/TimeStamp.h"
#include "nsSocketTransportService2.h"
#include "nsThreadUtils.h"
#include "prio.h"
#include "prnetdb.h"
#include "private/pprio.h"

using mozilla::TimeDuration;

using namespace mozilla::net;

namespace {

// A single fd's interest/result, decoupled from PRPollDesc: the composition
// under test here (WalkSocketLayers() + PollPollerBackend + UnmapReadyFlags)
// has no structural dependency on PRPollDesc's specific layout, only on
// PRFileDesc* (the legitimate, unavoidable NSPR object it walks/polls) and
// plain PR_POLL_* bits -- this type makes that visible in the test too.
struct FdInterest {
  PRFileDesc* fd = nullptr;
  int16_t in_flags = 0;
  int16_t out_flags = 0;
};

// Runs the equivalent of the production DoPollIterationWithBackend() flow
// over a single FdInterest array for one iteration: walks each entry's NSPR
// layer stack (WalkSocketLayers()), short-circuits the whole batch if any
// layer is already ready (matching Poller::Wait's old semantics), and
// otherwise registers everything with a fresh PollPollerBackend and waits.
static int32_t RunNewImpl(FdInterest* aEntries, uint32_t aCount,
                          const TimeDuration& aTimeout,
                          PollerStats* aStats = nullptr) {
  if (aCount == 0) {
    if (aStats) ++aStats->iterations;
    if (aTimeout != TimeDuration::Forever()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::max<int64_t>(0, int64_t(aTimeout.ToMilliseconds()))));
    }
    return 0;
  }

  std::vector<LayerWalkResult> results(aCount);
  bool anyShortCircuit = false;
  for (uint32_t i = 0; i < aCount; ++i) {
    if (!aEntries[i].fd || !aEntries[i].in_flags) continue;
    results[i] = WalkSocketLayers(aEntries[i].fd, aEntries[i].in_flags);
    if (results[i].shortCircuited) anyShortCircuit = true;
  }

  int32_t ready = 0;
  if (anyShortCircuit) {
    for (uint32_t i = 0; i < aCount; ++i) {
      if (results[i].shortCircuited) {
        aEntries[i].out_flags = results[i].outFlags;
        ++ready;
      } else {
        aEntries[i].out_flags = 0;
      }
    }
    if (aStats) {
      ++aStats->iterations;
      ++aStats->shortCircuits;
      aStats->readyCount.Add(static_cast<uint32_t>(ready));
    }
    return ready;
  }

  PollPollerBackend backend;
  for (uint32_t i = 0; i < aCount; ++i) {
    aEntries[i].out_flags = 0;
    if (!aEntries[i].fd || !aEntries[i].in_flags) continue;
    PollerFd fd = PR_FileDesc2NativeHandle(aEntries[i].fd);
    backend.Add(fd, results[i].osInterest,
               reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }

  nsTArray<PollerReadyEvent> readyEvents;
  ready = backend.Wait(aTimeout, readyEvents, aStats);
  for (auto& ev : readyEvents) {
    uint32_t idx = static_cast<uint32_t>(reinterpret_cast<intptr_t>(ev.key));
    aEntries[idx].out_flags =
        UnmapReadyFlags(ev.outFlags, results[idx].directionMap);
  }
  return ready;
}

// Converts an NSPR timeout to a TimeDuration -- needed only because this
// function also drives real PR_Poll, which requires PRIntervalTime.
static TimeDuration ToTimeDuration(PRIntervalTime aTimeout) {
  return aTimeout == PR_INTERVAL_NO_TIMEOUT
             ? TimeDuration::Forever()
             : TimeDuration::FromMilliseconds(
                   PR_IntervalToMilliseconds(aTimeout));
}

// Runs PR_Poll on `aNspr` and RunNewImpl() on `aNewer` and asserts they
// agree. out_flags is compared as a multiset (the ready-descriptor set is
// unordered).
//
// Both polls run on the STS thread; this suspends the STS's own
// DoPollIteration() so a concurrent Wait() on the PollableEvent cannot share
// macOS's process-wide kqueue and corrupt POLLHUP delivery.
static void RunBoth(PRPollDesc* aNspr, PRPollDesc* aNewer, uint32_t aCount,
                    PRIntervalTime aTimeout) {
  struct R {
    int32_t nNSPR = 0;
    int32_t nNew = 0;
  } r;

  std::vector<FdInterest> newer(aCount);
  for (uint32_t i = 0; i < aCount; ++i) {
    newer[i] = {aNewer[i].fd, aNewer[i].in_flags, 0};
  }

  MOZ_ASSERT(gSocketTransportService, "STS not running");
  NS_DispatchAndSpinEventLoopUntilComplete(
      "RunBoth"_ns, gSocketTransportService,
      NS_NewRunnableFunction("RunBoth", [&]() {
        r.nNSPR = PR_Poll(aNspr, aCount, aTimeout);
        r.nNew = RunNewImpl(newer.data(), aCount, ToTimeDuration(aTimeout));
      }));
  for (uint32_t i = 0; i < aCount; ++i) {
    aNewer[i].out_flags = newer[i].out_flags;
  }

  EXPECT_EQ(r.nNSPR, r.nNew) << "return values differ";
  if (r.nNSPR > 0 && r.nNew > 0) {
    std::vector<int16_t> nspr_flags, newer_flags;
    for (uint32_t i = 0; i < aCount; ++i) {
      if (aNspr[i].out_flags) nspr_flags.push_back(aNspr[i].out_flags);
      if (aNewer[i].out_flags) newer_flags.push_back(aNewer[i].out_flags);
    }
    std::sort(nspr_flags.begin(), nspr_flags.end());
    std::sort(newer_flags.begin(), newer_flags.end());
    EXPECT_EQ(nspr_flags, newer_flags) << "ready flag sets differ";
  }
}

// Connected TCP loopback socket pair. Both sockets are non-blocking after
// the connection is established. Works on all platforms.
struct SocketPair {
  PRFileDesc* a = nullptr;
  PRFileDesc* b = nullptr;

  SocketPair() {
    PRFileDesc* listener = PR_OpenTCPSocket(PR_AF_INET);
    if (!listener) return;

    PRNetAddr addr;
    memset(&addr, 0, sizeof(addr));
    PR_InitializeNetAddr(PR_IpAddrLoopback, 0, &addr);
    if (PR_Bind(listener, &addr) != PR_SUCCESS ||
        PR_GetSockName(listener, &addr) != PR_SUCCESS ||
        PR_Listen(listener, 1) != PR_SUCCESS) {
      PR_Close(listener);
      return;
    }

    b = PR_OpenTCPSocket(PR_AF_INET);
    if (!b) {
      PR_Close(listener);
      return;
    }
    if (PR_Connect(b, &addr, PR_SecondsToInterval(5)) != PR_SUCCESS) {
      PR_Close(listener);
      PR_Close(b);
      b = nullptr;
      return;
    }

    a = PR_Accept(listener, nullptr, PR_SecondsToInterval(5));
    PR_Close(listener);
    if (!a) {
      PR_Close(b);
      b = nullptr;
      return;
    }

    PRSocketOptionData nb;
    nb.option = PR_SockOpt_Nonblocking;
    nb.value.non_blocking = true;
    PR_SetSocketOption(a, &nb);
    PR_SetSocketOption(b, &nb);
  }

  bool Valid() const { return a && b; }

  ~SocketPair() {
    if (a) PR_Close(a);
    if (b) PR_Close(b);
  }

  SocketPair(const SocketPair&) = delete;
  SocketPair& operator=(const SocketPair&) = delete;
};

static void WriteBytes(PRFileDesc* aSock, const char* aBuf, int aLen) {
  int sent = 0;
  while (sent < aLen) {
    int n = PR_Send(aSock, aBuf + sent, aLen - sent, 0,
                    PR_MillisecondsToInterval(1000));
    if (n <= 0) break;
    sent += n;
  }
}


// Synthetic NSPR I/O layers for layer-walk tests.
static PRDescIdentity sShortCircuitId = PR_INVALID_IO_LAYER;
static PRDescIdentity sFlipId = PR_INVALID_IO_LAYER;
static PRIOMethods sShortCircuitMethods;
static PRIOMethods sFlipMethods;

static PRInt16 ShortCircuitPoll(PRFileDesc* /*fd*/, PRInt16 in_flags,
                                PRInt16* out_flags) {
  *out_flags = in_flags;
  return in_flags;
}

static PRInt16 FlipPoll(PRFileDesc* /*fd*/, PRInt16 in_flags,
                        PRInt16* out_flags) {
  *out_flags = 0;
  PRInt16 flipped = 0;
  if (in_flags & PR_POLL_READ) flipped |= PR_POLL_WRITE;
  if (in_flags & PR_POLL_WRITE) flipped |= PR_POLL_READ;
  return flipped;
}

static void InitLayers() {
  if (sShortCircuitId != PR_INVALID_IO_LAYER) return;
  sShortCircuitId = PR_GetUniqueIdentity("TestPoller::ShortCircuit");
  sFlipId = PR_GetUniqueIdentity("TestPoller::Flip");
  sShortCircuitMethods = *PR_GetDefaultIOMethods();
  sShortCircuitMethods.poll = ShortCircuitPoll;
  sFlipMethods = *PR_GetDefaultIOMethods();
  sFlipMethods.poll = FlipPoll;
}

static PRFileDesc* PushLayer(PRFileDesc* aFd, const PRIOMethods* aMethods,
                             PRDescIdentity aId) {
  PRFileDesc* layer = PR_CreateIOLayerStub(aId, aMethods);
  if (!layer) return nullptr;
  if (PR_PushIOLayer(aFd, PR_TOP_IO_LAYER, layer) != PR_SUCCESS) {
    PR_Close(layer);
    return nullptr;
  }
  return aFd;
}

}  // namespace

// Each test creates two independent fd sets (N for PR_Poll, P for the new
// backend), brings them into the same state via identical operations, then
// calls RunBoth (or runs both inline) and compares results.

TEST(TestPoller, EmptyArray) {
  RunBoth(nullptr, nullptr, 0, PR_INTERVAL_NO_WAIT);
}

TEST(TestPoller, TimeoutNoWait) {
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  PRPollDesc n{spN.a, PR_POLL_READ, 0};
  PRPollDesc p{spP.a, PR_POLL_READ, 0};
  RunBoth(&n, &p, 1, PR_INTERVAL_NO_WAIT);
}

TEST(TestPoller, SingleReadReady) {
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  char byte = 'x';
  WriteBytes(spN.b, &byte, 1);
  WriteBytes(spP.b, &byte, 1);

  PRPollDesc n{spN.a, PR_POLL_READ, 0};
  PRPollDesc p{spP.a, PR_POLL_READ, 0};
  RunBoth(&n, &p, 1, PR_MillisecondsToInterval(500));
}

TEST(TestPoller, SingleWriteReady) {
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  PRPollDesc n{spN.a, PR_POLL_WRITE, 0};
  PRPollDesc p{spP.a, PR_POLL_WRITE, 0};
  RunBoth(&n, &p, 1, PR_MillisecondsToInterval(100));
}

TEST(TestPoller, MultipleDescriptors) {
  SocketPair spN1, spN2, spN3;
  SocketPair spP1, spP2, spP3;
  ASSERT_TRUE(spN1.Valid() && spN2.Valid() && spN3.Valid());
  ASSERT_TRUE(spP1.Valid() && spP2.Valid() && spP3.Valid());

  char byte = 'y';
  WriteBytes(spN1.b, &byte, 1);
  WriteBytes(spP1.b, &byte, 1);

  PRPollDesc nspr[3] = {
      {spN1.a, PR_POLL_READ, 0},
      {spN2.a, PR_POLL_WRITE, 0},
      {spN3.a, PR_POLL_READ, 0},
  };
  PRPollDesc newer[3] = {
      {spP1.a, PR_POLL_READ, 0},
      {spP2.a, PR_POLL_WRITE, 0},
      {spP3.a, PR_POLL_READ, 0},
  };
  RunBoth(nspr, newer, 3, PR_MillisecondsToInterval(200));
}

TEST(TestPoller, HupOnDisconnect) {
  struct HupResult {
    int32_t nNSPR = 0;
    int32_t nNew = 0;
    PRPollDesc n{};
    FdInterest p{};
  } res;

  MOZ_ASSERT(gSocketTransportService, "STS not running");
  NS_DispatchAndSpinEventLoopUntilComplete(
      "HupOnDisconnect"_ns, gSocketTransportService,
      NS_NewRunnableFunction("HupOnDisconnect", [&res]() {
        // Two simultaneously-live pairs so the fds don't collide.
        SocketPair spN, spP;
        if (!spN.Valid() || !spP.Valid()) return;

        PRFileDesc* nFd = spN.a; spN.a = nullptr;
        PRFileDesc* pFd = spP.a; spP.a = nullptr;
        PR_Close(spN.b); spN.b = nullptr;
        PR_Close(spP.b); spP.b = nullptr;

        // Wait until POLLIN is visible on both fds before measuring.
        PRPollDesc settle[2] = {{nFd, PR_POLL_READ, 0}, {pFd, PR_POLL_READ, 0}};
        while (PR_Poll(settle, 2, PR_MillisecondsToInterval(500)) == 0) {}

        res.n = {nFd, PR_POLL_READ, 0};
        res.p = {pFd, PR_POLL_READ, 0};
        res.nNSPR = PR_Poll(&res.n, 1, PR_INTERVAL_NO_WAIT);
        res.nNew = RunNewImpl(&res.p, 1, TimeDuration::FromSeconds(0));
        PR_Close(nFd);
        PR_Close(pFd);
      }));

  EXPECT_EQ(res.nNSPR, res.nNew) << "return values differ";
  if (res.nNSPR > 0 && res.nNew > 0) {
    // macOS's poll(2)/kqueue reports POLLHUP non-deterministically across
    // consecutive calls on a freshly-disconnected TCP socket — both impls
    // correctly map whatever the kernel returns, so mask the bit there.
    // NSPR's PR_Poll on Windows (w32poll.c) is implemented on top of
    // select(), which has no HUP concept at all: a disconnected socket only
    // ever comes back as FD_ISSET on the read set, i.e. plain PR_POLL_READ.
    // res.n can therefore never carry PR_POLL_HUP on Windows, regardless of
    // what the new WSAPoll-based backend correctly reports in res.p, so mask
    // it there too. Linux's POLLHUP delivery is deterministic; the bit must
    // match there.
#if defined(XP_DARWIN) || defined(XP_WIN)
    constexpr int16_t kMask = ~PR_POLL_HUP;
#else
    constexpr int16_t kMask = ~int16_t{0};
#endif
    EXPECT_EQ(res.n.out_flags & kMask, res.p.out_flags & kMask)
        << "ready flag sets differ";
  }
}

TEST(TestPoller, NullFdIgnored) {
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  char byte = 'z';
  WriteBytes(spN.b, &byte, 1);
  WriteBytes(spP.b, &byte, 1);

  PRPollDesc nspr[2] = {{nullptr, PR_POLL_READ, 0}, {spN.a, PR_POLL_READ, 0}};
  PRPollDesc newer[2] = {{nullptr, PR_POLL_READ, 0}, {spP.a, PR_POLL_READ, 0}};
  RunBoth(nspr, newer, 2, PR_MillisecondsToInterval(500));
}

TEST(TestPoller, ZeroInFlagsIgnored) {
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  char byte = 'w';
  WriteBytes(spN.b, &byte, 1);
  WriteBytes(spP.b, &byte, 1);

  PRPollDesc nspr[2] = {{spN.a, 0, 0}, {spN.a, PR_POLL_READ, 0}};
  PRPollDesc newer[2] = {{spP.a, 0, 0}, {spP.a, PR_POLL_READ, 0}};
  RunBoth(nspr, newer, 2, PR_MillisecondsToInterval(500));
}

TEST(TestPoller, LayerShortCircuit) {
  InitLayers();
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  PRFileDesc* fdN = PushLayer(spN.a, &sShortCircuitMethods, sShortCircuitId);
  PRFileDesc* fdP = PushLayer(spP.a, &sShortCircuitMethods, sShortCircuitId);
  ASSERT_TRUE(fdN && fdP);
  spN.a = nullptr;
  spP.a = nullptr;

  PRPollDesc n{fdN, PR_POLL_READ, 0};
  PRPollDesc p{fdP, PR_POLL_READ, 0};
  RunBoth(&n, &p, 1, PR_INTERVAL_NO_WAIT);

  PR_Close(fdN);
  PR_Close(fdP);
}

TEST(TestPoller, LayerShortCircuitDoesNotBlock) {
  InitLayers();
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  PRFileDesc* fdN = PushLayer(spN.a, &sShortCircuitMethods, sShortCircuitId);
  PRFileDesc* fdP = PushLayer(spP.a, &sShortCircuitMethods, sShortCircuitId);
  ASSERT_TRUE(fdN && fdP);
  spN.a = nullptr;
  spP.a = nullptr;

  PRPollDesc n{fdN, PR_POLL_READ | PR_POLL_WRITE, 0};
  PRPollDesc p{fdP, PR_POLL_READ | PR_POLL_WRITE, 0};
  RunBoth(&n, &p, 1, PR_INTERVAL_NO_WAIT);

  PR_Close(fdN);
  PR_Close(fdP);
}

TEST(TestPoller, LayerFlipReadToWrite) {
  InitLayers();
  SocketPair spN, spP;
  ASSERT_TRUE(spN.Valid() && spP.Valid());

  char byte = 'f';
  WriteBytes(spN.b, &byte, 1);
  WriteBytes(spP.b, &byte, 1);

  PRFileDesc* fdN = PushLayer(spN.a, &sFlipMethods, sFlipId);
  PRFileDesc* fdP = PushLayer(spP.a, &sFlipMethods, sFlipId);
  ASSERT_TRUE(fdN && fdP);
  spN.a = nullptr;
  spP.a = nullptr;

  PRPollDesc n{fdN, PR_POLL_WRITE, 0};
  PRPollDesc p{fdP, PR_POLL_WRITE, 0};
  RunBoth(&n, &p, 1, PR_MillisecondsToInterval(500));

  PR_Close(fdN);
  PR_Close(fdP);
}

TEST(TestPoller, CrossDescriptorShortCircuit) {
  InitLayers();
  SocketPair spN1, spN2;
  SocketPair spP1, spP2;
  ASSERT_TRUE(spN1.Valid() && spN2.Valid());
  ASSERT_TRUE(spP1.Valid() && spP2.Valid());

  PRFileDesc* scN = PushLayer(spN2.a, &sShortCircuitMethods, sShortCircuitId);
  PRFileDesc* scP = PushLayer(spP2.a, &sShortCircuitMethods, sShortCircuitId);
  ASSERT_TRUE(scN && scP);
  spN2.a = nullptr;
  spP2.a = nullptr;

  PRPollDesc nspr[2] = {{spN1.a, PR_POLL_READ, 0}, {scN, PR_POLL_READ, 0}};
  PRPollDesc newer[2] = {{spP1.a, PR_POLL_READ, 0}, {scP, PR_POLL_READ, 0}};
  RunBoth(nspr, newer, 2, PR_INTERVAL_NO_WAIT);

  PR_Close(scN);
  PR_Close(scP);
}

TEST(TestPoller, HistogramAdd) {
  Histogram h;
  EXPECT_TRUE(h.Empty());
  EXPECT_EQ(h.Percentile(0.5), 0u);

  h.Add(0);
  EXPECT_FALSE(h.Empty());
  EXPECT_EQ(h.mCount, 1u);
  EXPECT_EQ(h.mBuckets[0], 1u);
  EXPECT_EQ(h.Percentile(0.5), 0u);

  h.Add(1);
  EXPECT_EQ(h.mBuckets[1], 1u);

  h.Add(2);
  h.Add(3);
  EXPECT_EQ(h.mBuckets[2], 2u);

  h.Add(1000000);
  EXPECT_EQ(h.mMax, 1000000u);
}

TEST(TestPoller, PollerStatsAccumulate) {
  SocketPair sp;
  ASSERT_TRUE(sp.Valid());

  char byte = 's';
  WriteBytes(sp.b, &byte, 1);

  PollerStats stats;
  EXPECT_EQ(stats.iterations, 0u);

  FdInterest desc{sp.a, PR_POLL_READ, 0};
  int32_t n = RunNewImpl(&desc, 1, TimeDuration::FromMilliseconds(500), &stats);
  EXPECT_GT(n, 0);

  EXPECT_EQ(stats.iterations, 1u);
  EXPECT_EQ(stats.fdCount.mCount, 1u);
  EXPECT_GT(stats.readyCount.mCount, 0u);
}
