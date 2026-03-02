// vim:set sw=2 sts=2 et cin:
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSocketTransportService2.h"

#include "mozilla/Atomics.h"
#include "mozilla/ChaosMode.h"
#include "mozilla/glean/NetwerkMetrics.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/MaybeLeakRefPtr.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Preferences.h"
#include "mozilla/ProfilerMarkers.h"
#include "mozilla/ProfilerThreadSleep.h"
#include "mozilla/PublicSSL.h"
#include "mozilla/ReverseIterator.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/Tokenizer.h"
#include "mozilla/Telemetry.h"
#include "nsASocketHandler.h"
#include "nsError.h"
#include "nsIEventTarget.h"
#include "nsIFile.h"
#include "nsINetworkLinkService.h"
#include "nsIOService.h"
#include "nsIObserverService.h"
#include "nsIWidget.h"
#include "nsServiceManagerUtils.h"
#include "nsSocketTransport2.h"
#include "nsThreadUtils.h"
#include "prerror.h"
#include "prnetdb.h"

namespace mozilla {
namespace net {

#define SOCKET_THREAD_LONGTASK_MS 3

LazyLogModule gSocketTransportLog("nsSocketTransport");
LazyLogModule gUDPSocketLog("UDPSocket");
LazyLogModule gTCPSocketLog("TCPSocket");

nsSocketTransportService* gSocketTransportService = nullptr;
static Atomic<PRThread*, Relaxed> gSocketThread(nullptr);

#define SEND_BUFFER_PREF "network.tcp.sendbuffer"
#define KEEPALIVE_ENABLED_PREF "network.tcp.keepalive.enabled"
#define KEEPALIVE_IDLE_TIME_PREF "network.tcp.keepalive.idle_time"
#define KEEPALIVE_RETRY_INTERVAL_PREF "network.tcp.keepalive.retry_interval"
#define KEEPALIVE_PROBE_COUNT_PREF "network.tcp.keepalive.probe_count"
#define SOCKET_LIMIT_TARGET 1000U
#define MAX_TIME_BETWEEN_TWO_POLLS \
  "network.sts.max_time_for_events_between_two_polls"
#define POLL_BUSY_WAIT_PERIOD "network.sts.poll_busy_wait_period"
#define POLL_BUSY_WAIT_PERIOD_TIMEOUT \
  "network.sts.poll_busy_wait_period_timeout"
#define MAX_TIME_FOR_PR_CLOSE_DURING_SHUTDOWN \
  "network.sts.max_time_for_pr_close_during_shutdown"

uint32_t nsSocketTransportService::gMaxCount;
PRCallOnceType nsSocketTransportService::gMaxCountInitOnce;

// Utility functions
bool OnSocketThread() { return PR_GetCurrentThread() == gSocketThread; }

//-----------------------------------------------------------------------------

bool nsSocketTransportService::SocketContext::IsTimedOut(
    PRIntervalTime now) const {
  return TimeoutIn(now) == 0;
}

void nsSocketTransportService::SocketContext::EnsureTimeout(
    PRIntervalTime now) {
  SOCKET_LOG(("SocketContext::EnsureTimeout socket=%p", mHandler.get()));
  if (!mPollStartEpoch) {
    SOCKET_LOG(("  engaging"));
    mPollStartEpoch = now;
  }
}

void nsSocketTransportService::SocketContext::DisengageTimeout() {
  SOCKET_LOG(("SocketContext::DisengageTimeout socket=%p", mHandler.get()));
  mPollStartEpoch = 0;
}

PRIntervalTime nsSocketTransportService::SocketContext::TimeoutIn(
    PRIntervalTime now) const {
  SOCKET_LOG(("SocketContext::TimeoutIn socket=%p, timeout=%us", mHandler.get(),
              mHandler->mPollTimeout));

  if (mHandler->mPollTimeout == UINT16_MAX || !mPollStartEpoch) {
    SOCKET_LOG(("  not engaged"));
    return NS_SOCKET_POLL_TIMEOUT;
  }

  PRIntervalTime elapsed = (now - mPollStartEpoch);
  PRIntervalTime timeout = PR_SecondsToInterval(mHandler->mPollTimeout);

  if (elapsed >= timeout) {
    SOCKET_LOG(("  timed out!"));
    return 0;
  }
  SOCKET_LOG(("  remains %us", PR_IntervalToSeconds(timeout - elapsed)));
  return timeout - elapsed;
}

void nsSocketTransportService::SocketContext::MaybeResetEpoch() {
  if (mPollStartEpoch && mHandler->mPollTimeout == UINT16_MAX) {
    mPollStartEpoch = 0;
  }
}

//-----------------------------------------------------------------------------
// ctor/dtor (called on the main/UI thread by the service manager)

nsSocketTransportService::nsSocketTransportService()
    // Pre-allocate socket lists to SOCKET_LIMIT_TARGET to prevent reallocation.
    // We store SocketContext* pointers as keys in the poller, so addresses must
    // remain stable.
    : mActiveList(SOCKET_LIMIT_TARGET),
      mIdleList(SOCKET_LIMIT_TARGET),
      mPoller(necko_poll_new()),
      mMaxTimeForPrClosePref(PR_SecondsToInterval(5)),
      mNetworkLinkChangeBusyWaitPeriod(PR_SecondsToInterval(50)),
      mNetworkLinkChangeBusyWaitTimeout(PR_SecondsToInterval(7)) {
  MOZ_ASSERT(mPoller, "necko_poll_new failed");
  NS_ASSERTION(NS_IsMainThread(), "wrong thread");

  PR_CallOnce(&gMaxCountInitOnce, DiscoverMaxCount);

  NS_ASSERTION(!gSocketTransportService, "must not instantiate twice");
  gSocketTransportService = this;
}

void nsSocketTransportService::ApplyPortRemap(uint16_t* aPort) {
  MOZ_ASSERT(IsOnCurrentThreadInfallible());

  if (!mPortRemapping) {
    return;
  }

  // Reverse the array to make later rules override earlier rules.
  for (auto const& portMapping : Reversed(*mPortRemapping)) {
    if (*aPort < std::get<0>(portMapping)) {
      continue;
    }
    if (*aPort > std::get<1>(portMapping)) {
      continue;
    }

    *aPort = std::get<2>(portMapping);
    return;
  }
}

bool nsSocketTransportService::UpdatePortRemapPreference(
    nsACString const& aPortMappingPref) {
  TPortRemapping portRemapping;

  auto consumePreference = [&]() -> bool {
    Tokenizer tokenizer(aPortMappingPref);

    tokenizer.SkipWhites();
    if (tokenizer.CheckEOF()) {
      return true;
    }

    nsTArray<std::tuple<uint16_t, uint16_t>> ranges(2);
    while (true) {
      uint16_t loPort;
      tokenizer.SkipWhites();
      if (!tokenizer.ReadInteger(&loPort)) {
        break;
      }

      uint16_t hiPort;
      tokenizer.SkipWhites();
      if (tokenizer.CheckChar('-')) {
        tokenizer.SkipWhites();
        if (!tokenizer.ReadInteger(&hiPort)) {
          break;
        }
      } else {
        hiPort = loPort;
      }

      ranges.AppendElement(std::make_tuple(loPort, hiPort));

      tokenizer.SkipWhites();
      if (tokenizer.CheckChar(',')) {
        continue;  // another port or port range is expected
      }

      if (tokenizer.CheckChar('=')) {
        uint16_t targetPort;
        tokenizer.SkipWhites();
        if (!tokenizer.ReadInteger(&targetPort)) {
          break;
        }

        // Storing reversed, because the most common cases (like 443) will very
        // likely be listed as first, less common cases will be added to the end
        // of the list mapping to the same port. As we iterate the whole
        // remapping array from the end, this may have a small perf win by
        // hitting the most common cases earlier.
        for (auto const& range : Reversed(ranges)) {
          portRemapping.AppendElement(std::make_tuple(
              std::get<0>(range), std::get<1>(range), targetPort));
        }
        ranges.Clear();

        tokenizer.SkipWhites();
        if (tokenizer.CheckChar(';')) {
          continue;  // more mappings (or EOF) expected
        }
        if (tokenizer.CheckEOF()) {
          return true;
        }
      }

      // Anything else is unexpected.
      break;
    }

    // 'break' from the parsing loop means ill-formed preference
    portRemapping.Clear();
    return false;
  };

  bool rv = consumePreference();

  if (!IsOnCurrentThread()) {
    nsCOMPtr<nsIThread> thread = GetThreadSafely();
    if (!thread) {
      // Init hasn't been called yet. Could probably just assert.
      // If shutdown, the dispatch below will just silently fail.
      NS_ASSERTION(false, "ApplyPortRemapPreference before STS::Init");
      return false;
    }
    thread->Dispatch(NewRunnableMethod<TPortRemapping>(
        "net::ApplyPortRemapping", this,
        &nsSocketTransportService::ApplyPortRemapPreference, portRemapping));
  } else {
    ApplyPortRemapPreference(portRemapping);
  }

  return rv;
}

nsSocketTransportService::~nsSocketTransportService() {
  NS_ASSERTION(NS_IsMainThread(), "wrong thread");
  NS_ASSERTION(!mInitialized, "not shutdown properly");

  mPoller = nullptr;
  gSocketTransportService = nullptr;
}

//-----------------------------------------------------------------------------
// event queue (any thread)

already_AddRefed<nsIThread> nsSocketTransportService::GetThreadSafely() {
  MutexAutoLock lock(mLock);
  nsCOMPtr<nsIThread> result = mThread;
  return result.forget();
}

NS_IMETHODIMP
nsSocketTransportService::DispatchFromScript(nsIRunnable* event,
                                             DispatchFlags flags) {
  return Dispatch(do_AddRef(event), flags);
}

NS_IMETHODIMP
nsSocketTransportService::Dispatch(already_AddRefed<nsIRunnable> event,
                                   DispatchFlags flags) {
  // NOTE: We don't leak runnables on dispatch failure here, even if
  // NS_DISPATCH_FALLIBLE is not specified.
  nsCOMPtr<nsIRunnable> event_ref(std::move(event));
  SOCKET_LOG(("STS dispatch [%p]\n", event_ref.get()));

  nsCOMPtr<nsIThread> thread = GetThreadSafely();
  if (!thread) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  nsresult rv = NS_OK;
  bool isHighPriority = false;
  if (StaticPrefs::network_socket_prioritize_runnables()) {
    if (nsCOMPtr<nsIRunnablePriority> p = do_QueryInterface(event_ref)) {
      uint32_t priority = nsIRunnablePriority::PRIORITY_NORMAL;
      p->GetPriority(&priority);
      if (priority > nsIRunnablePriority::PRIORITY_NORMAL) {
        isHighPriority = true;
      }
    }
  }

  if (isHighPriority) {
    // Add to priority queue instead of dispatching to thread
    AutoWriteLock lock(mQueueLock);
    mPriorityEventQueue.Push(event_ref.forget());
    // Wake the socket thread so it processes the priority event.
    OnDispatchedEvent();
  } else {
    rv = thread->Dispatch(event_ref.forget(), flags | NS_DISPATCH_FALLIBLE);
  }

  if (rv == NS_ERROR_UNEXPECTED) {
    // Thread is no longer accepting events. We must have just shut it
    // down on the main thread. Pretend we never saw it.
    rv = NS_ERROR_NOT_INITIALIZED;
  }
  return rv;
}

NS_IMETHODIMP
nsSocketTransportService::DelayedDispatch(already_AddRefed<nsIRunnable>,
                                          uint32_t) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsSocketTransportService::RegisterShutdownTask(nsITargetShutdownTask* task) {
  nsCOMPtr<nsIThread> thread = GetThreadSafely();
  return thread ? thread->RegisterShutdownTask(task) : NS_ERROR_UNEXPECTED;
}

NS_IMETHODIMP
nsSocketTransportService::UnregisterShutdownTask(nsITargetShutdownTask* task) {
  nsCOMPtr<nsIThread> thread = GetThreadSafely();
  return thread ? thread->UnregisterShutdownTask(task) : NS_ERROR_UNEXPECTED;
}

nsIEventTarget::FeatureFlags nsSocketTransportService::GetFeatures() {
  nsCOMPtr<nsIThread> thread = GetThreadSafely();
  nsIEventTarget::FeatureFlags flags = nsIEventTarget::SUPPORTS_BASE;
  if (thread) {
    flags = thread->GetFeatures();
  }

  if (XRE_IsParentProcess()) {
    flags |= SUPPORTS_PRIORITIZATION;
  }

  return flags;
}

NS_IMETHODIMP
nsSocketTransportService::IsOnCurrentThread(bool* result) {
  *result = OnSocketThread();
  return NS_OK;
}

NS_IMETHODIMP_(bool)
nsSocketTransportService::IsOnCurrentThreadInfallible() {
  return OnSocketThread();
}

//-----------------------------------------------------------------------------
// nsIDirectTaskDispatcher

already_AddRefed<nsIDirectTaskDispatcher>
nsSocketTransportService::GetDirectTaskDispatcherSafely() {
  MutexAutoLock lock(mLock);
  nsCOMPtr<nsIDirectTaskDispatcher> result = mDirectTaskDispatcher;
  return result.forget();
}

NS_IMETHODIMP
nsSocketTransportService::DispatchDirectTask(
    already_AddRefed<nsIRunnable> aEvent) {
  nsCOMPtr<nsIDirectTaskDispatcher> dispatcher =
      GetDirectTaskDispatcherSafely();
  NS_ENSURE_TRUE(dispatcher, NS_ERROR_NOT_INITIALIZED);
  return dispatcher->DispatchDirectTask(std::move(aEvent));
}

NS_IMETHODIMP nsSocketTransportService::DrainDirectTasks() {
  nsCOMPtr<nsIDirectTaskDispatcher> dispatcher =
      GetDirectTaskDispatcherSafely();
  if (!dispatcher) {
    // nothing to drain.
    return NS_OK;
  }
  return dispatcher->DrainDirectTasks();
}

NS_IMETHODIMP nsSocketTransportService::HaveDirectTasks(bool* aValue) {
  nsCOMPtr<nsIDirectTaskDispatcher> dispatcher =
      GetDirectTaskDispatcherSafely();
  if (!dispatcher) {
    *aValue = false;
    return NS_OK;
  }
  return dispatcher->HaveDirectTasks(aValue);
}

//-----------------------------------------------------------------------------
// socket api (socket thread only)

NS_IMETHODIMP
nsSocketTransportService::NotifyWhenCanAttachSocket(nsIRunnable* event) {
  SOCKET_LOG(("nsSocketTransportService::NotifyWhenCanAttachSocket\n"));

  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  if (CanAttachSocket()) {
    return Dispatch(event, NS_DISPATCH_NORMAL);
  }

  auto* runnable = new LinkedRunnableEvent(event);
  mPendingSocketQueue.insertBack(runnable);
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::AttachSocket(PRFileDesc* fd,
                                       nsASocketHandler* handler) {
  SOCKET_LOG(
      ("nsSocketTransportService::AttachSocket [handler=%p]\n", handler));
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  if (!CanAttachSocket()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  SocketContext sock{fd, handler, 0};

  AddToIdleList(&sock);
  return NS_OK;
}

// The number of sockets that can be attached at any given time is
// limited. AttachSocket will fail if the limit is exceeded. Consumers
// should call CanAttachSocket and check the result before creating a
// socket.

bool nsSocketTransportService::CanAttachSocket() {
  MOZ_ASSERT(!mShuttingDown);
  uint32_t total = mActiveList.Length() + mIdleList.Length();
  bool rv = total < gMaxCount;

  if (!rv) {
    static bool reported_socket_limit_reached = false;
    if (!reported_socket_limit_reached) {
      mozilla::glean::networking::os_socket_limit_reached.Add(1);
      reported_socket_limit_reached = true;
    }
    SOCKET_LOG(
        ("nsSocketTransportService::CanAttachSocket failed -  total: %d, "
         "maxCount: %d\n",
         total, gMaxCount));
  }

  MOZ_ASSERT(mInitialized);
  return rv;
}

nsresult nsSocketTransportService::DetachSocket(SocketContextList& listHead,
                                                SocketContext* sock) {
  SOCKET_LOG(("nsSocketTransportService::DetachSocket [handler=%p]\n",
              sock->mHandler.get()));
  MOZ_ASSERT((&listHead == &mActiveList) || (&listHead == &mIdleList),
             "DetachSocket invalid head");

  bool wasActive = (&listHead == &mActiveList);
  if (wasActive) {
    // Remove from poller before OnSocketDetached releases the fd.
    MOZ_ASSERT(sock->mIsRegisteredWithPoller,
               "active socket should be registered with poller");
    PollDelete(sock->mNativeFD);
    sock->mIsRegisteredWithPoller = false;
  }

  // Inform the handler that this socket is going away.
  sock->mHandler->OnSocketDetached(sock->mFD);
  mSentBytesCount += sock->mHandler->ByteCountSent();
  mReceivedBytesCount += sock->mHandler->ByteCountReceived();

  // cleanup
  sock->mFD = nullptr;

  if (wasActive) {
    RemoveFromPollList(sock);
  } else {
    RemoveFromIdleList(sock);
  }

  // NOTE: sock is now an invalid pointer

  //
  // notify the first element on the pending socket queue...
  //
  nsCOMPtr<nsIRunnable> event;
  LinkedRunnableEvent* runnable = mPendingSocketQueue.getFirst();
  if (runnable) {
    event = runnable->TakeEvent();
    runnable->remove();
    delete runnable;
  }
  if (event) {
    // move event from pending queue to dispatch queue
    return Dispatch(event, NS_DISPATCH_NORMAL);
  }
  return NS_OK;
}

// Returns the index of a SocketContext within a list, or -1 if it's
// not a pointer to a list element
// NOTE: this could be supplied by nsTArray<>
int64_t nsSocketTransportService::SockIndex(SocketContextList& aList,
                                            SocketContext* aSock) {
  ptrdiff_t index = -1;
  if (!aList.IsEmpty()) {
    index = aSock - &aList[0];
    if (index < 0 || (size_t)index + 1 > aList.Length()) {
      index = -1;
    }
  }
  return (int64_t)index;
}

void nsSocketTransportService::AddToPollList(SocketContext* sock) {
  MOZ_ASSERT(SockIndex(mActiveList, sock) == -1,
             "AddToPollList Socket Already Active");

  SOCKET_LOG(("nsSocketTransportService::AddToPollList %p [handler=%p]\n", sock,
              sock->mHandler.get()));

  sock->EnsureTimeout(PR_IntervalNow());

  // We store SocketContext* pointers as keys in the poller for O(1) event
  // lookup. Constructor pre-allocates to SOCKET_LIMIT_TARGET, and
  // CanAttachSocket limits total sockets to gMaxCount (≤ SOCKET_LIMIT_TARGET),
  // so reallocation cannot occur. ChaosMode InsertElementAt shifts elements,
  // requiring rekey.
  [[maybe_unused]] SocketContext* oldBase =
      mActiveList.IsEmpty() ? nullptr : &mActiveList[0];
  size_t oldLen = mActiveList.Length();
  size_t index;
  if (ChaosMode::isActive(ChaosFeature::NetworkScheduling)) {
    auto newSocketIndex = mActiveList.Length();
    newSocketIndex = ChaosMode::randomUint32LessThan(newSocketIndex + 1);
    mActiveList.InsertElementAt(
        newSocketIndex, SocketContext(sock->mFD, sock->mHandler.forget(),
                                      sock->mPollStartEpoch));
    index = newSocketIndex;
    // InsertElementAt shifts elements at index+1..end - update their keys.
    if (index < oldLen) {
      for (size_t i = index + 1; i < mActiveList.Length(); ++i) {
        SocketContext& s = mActiveList[i];
        if (s.mIsRegisteredWithPoller) {
          [[maybe_unused]] PollResult result = necko_poll_rekey(
              mPoller.get(), s.mNativeFD, reinterpret_cast<uintptr_t>(&s));
          MOZ_ASSERT(result == PollResult::Ok, "necko_poll_rekey failed");
        }
      }
    }
  } else {
    // Avoid refcount bump/decrease
    mActiveList.EmplaceBack(sock->mFD, sock->mHandler.forget(),
                            sock->mPollStartEpoch);
    index = mActiveList.Length() - 1;
    MOZ_ASSERT(mActiveList[index].mFD == sock->mFD, "index incorrect");
  }

  // Sanity check: reallocation should be impossible since the constructor
  // pre-allocates and CanAttachSocket limits total sockets.
  [[maybe_unused]] SocketContext* newBase = &mActiveList[0];
  MOZ_ASSERT(mActiveList.IsEmpty() || oldBase == nullptr || oldBase == newBase,
             "mActiveList reallocated unexpectedly");

  PollLayersAndRegister(
      &mActiveList[index],
      static_cast<int16_t>(mActiveList[index].mHandler->mPollFlags), nullptr);
  SOCKET_LOG(
      ("  active=%zu idle=%zu\n", mActiveList.Length(), mIdleList.Length()));
}

int64_t nsSocketTransportService::SwapRemoveElementAt(SocketContextList& aList,
                                                      int64_t aIndex) {
  // We use manual swap + RemoveLastElement instead of UnorderedRemoveElementAt
  // because nsTArray::UnorderedRemoveElementAt frees the buffer when the array
  // becomes empty, which would make it reallocate on the next insert and
  // invalidate all SocketContext* pointers used as keys in the poller.
  auto lastIndex = static_cast<int64_t>(aList.Length() - 1);
  if (aIndex != lastIndex) {
    std::swap(aList[aIndex], aList[lastIndex]);
    aList.RemoveLastElement();
    return aIndex;
  }
  aList.RemoveLastElement();
  return -1;
}

void nsSocketTransportService::RemoveFromPollList(SocketContext* sock) {
  SOCKET_LOG(("nsSocketTransportService::RemoveFromPollList %p [handler=%p]\n",
              sock, sock->mHandler.get()));

  auto index = SockIndex(mActiveList, sock);
  MOZ_RELEASE_ASSERT(index != -1, "invalid index");

  SOCKET_LOG(("  index=%" PRId64 " mActiveList.Length()=%zu\n", index,
              mActiveList.Length()));
  // May already be unregistered when called from DetachSocket, which does
  // an early PollDelete before OnSocketDetached can release the fd.
  if (mActiveList[index].mIsRegisteredWithPoller) {
    PollDelete(mActiveList[index].mNativeFD);
    mActiveList[index].mIsRegisteredWithPoller = false;
  }

  int64_t movedIndex = SwapRemoveElementAt(mActiveList, index);
  if (movedIndex >= 0) {
    SocketContext& moved = mActiveList[movedIndex];
    MOZ_ASSERT(moved.mIsRegisteredWithPoller,
               "moved socket should be registered with poller");
    [[maybe_unused]] PollResult result = necko_poll_rekey(
        mPoller.get(), moved.mNativeFD, reinterpret_cast<uintptr_t>(&moved));
    MOZ_ASSERT(result == PollResult::Ok, "necko_poll_rekey failed");
  }
  SOCKET_LOG(
      ("  active=%zu idle=%zu\n", mActiveList.Length(), mIdleList.Length()));
}

void nsSocketTransportService::AddToIdleList(SocketContext* sock) {
  MOZ_ASSERT(SockIndex(mIdleList, sock) == -1,
             "AddToIdleList Socket Already Idle");

  SOCKET_LOG(("nsSocketTransportService::AddToIdleList %p [handler=%p]\n", sock,
              sock->mHandler.get()));

  // Avoid refcount bump/decrease
  mIdleList.EmplaceBack(sock->mFD, sock->mHandler.forget(),
                        sock->mPollStartEpoch);

  SOCKET_LOG(
      ("  active=%zu idle=%zu\n", mActiveList.Length(), mIdleList.Length()));
}

void nsSocketTransportService::RemoveFromIdleList(SocketContext* sock) {
  SOCKET_LOG(("nsSocketTransportService::RemoveFromIdleList [handler=%p]\n",
              sock->mHandler.get()));
  auto index = SockIndex(mIdleList, sock);
  MOZ_RELEASE_ASSERT(index != -1);
  SwapRemoveElementAt(mIdleList, index);

  SOCKET_LOG(
      ("  active=%zu idle=%zu\n", mActiveList.Length(), mIdleList.Length()));
}

void nsSocketTransportService::MoveToIdleList(SocketContext* sock) {
  SOCKET_LOG(("nsSocketTransportService::MoveToIdleList %p [handler=%p]\n",
              sock, sock->mHandler.get()));
  MOZ_ASSERT(SockIndex(mIdleList, sock) == -1);
  MOZ_ASSERT(SockIndex(mActiveList, sock) != -1);
  AddToIdleList(sock);
  RemoveFromPollList(sock);
}

void nsSocketTransportService::MoveToPollList(SocketContext* sock) {
  SOCKET_LOG(("nsSocketTransportService::MoveToPollList %p [handler=%p]\n",
              sock, sock->mHandler.get()));
  MOZ_ASSERT(SockIndex(mIdleList, sock) != -1);
  MOZ_ASSERT(SockIndex(mActiveList, sock) == -1);
  AddToPollList(sock);
  RemoveFromIdleList(sock);
}

void nsSocketTransportService::ApplyPortRemapPreference(
    TPortRemapping const& portRemapping) {
  MOZ_ASSERT(IsOnCurrentThreadInfallible());

  mPortRemapping.reset();
  if (!portRemapping.IsEmpty()) {
    mPortRemapping.emplace(portRemapping);
  }
}

void nsSocketTransportService::PollDelete(PROsfd aFD) {
  [[maybe_unused]] PollResult result = necko_poll_delete(mPoller.get(), aFD);
  MOZ_ASSERT(result == PollResult::Ok, "necko_poll_delete failed");
}

int32_t nsSocketTransportService::Poll(PRIntervalTime ts,
                                       PRIntervalTime aSocketTimeout) {
  MOZ_ASSERT(IsOnCurrentThread());
  int64_t pollTimeout;  // -1 = infinite, 0 = non-blocking, >0 = milliseconds

  bool pendingEvents = false;
  mRawThread->HasPendingEvents(&pendingEvents);
  {
    AutoReadLock lock(mQueueLock);
    pendingEvents = pendingEvents || !mPriorityEventQueue.IsEmpty();
  }

  if (pendingEvents || aSocketTimeout == PR_INTERVAL_NO_WAIT) {
    pollTimeout = 0;
  } else if (aSocketTimeout == PR_INTERVAL_NO_TIMEOUT) {
    pollTimeout = -1;
  } else {
    pollTimeout = PR_IntervalToMilliseconds(aSocketTimeout);
  }

  if ((ts - mLastNetworkLinkChangeTime) < mNetworkLinkChangeBusyWaitPeriod) {
    // Being here means we are few seconds after a network change has
    // been detected.
    int64_t busyWaitTimeoutMs =
        PR_IntervalToMilliseconds(mNetworkLinkChangeBusyWaitTimeout);
    if (busyWaitTimeoutMs > 0 &&
        (pollTimeout < 0 || busyWaitTimeoutMs < pollTimeout)) {
      pollTimeout = busyWaitTimeoutMs;
      SOCKET_LOG(("  timeout shortened after network change event"));
    }
  }

  TimeStamp pollStart;
  if (Telemetry::CanRecordPrereleaseData()) {
    pollStart = TimeStamp::NowLoRes();
  }

  SOCKET_LOG(("    timeout = %" PRId64 " milliseconds\n", pollTimeout));

  int32_t n;
  {
#ifdef MOZ_GECKO_PROFILER
    TimeStamp startTime = TimeStamp::Now();
    if (pollTimeout != 0) {
      // There will be an actual non-zero wait, let the profiler know about it
      // by marking thread as sleeping around the polling call.
      profiler_thread_sleep();
    }
#endif

    n = necko_poll_wait(mPoller.get(), &mPolledEvents, pollTimeout);

#ifdef MOZ_GECKO_PROFILER
    if (pollTimeout != 0) {
      profiler_thread_wake();
    }
    if (profiler_thread_is_being_profiled_for_markers()) {
      uint32_t pollCount = necko_poll_len(mPoller.get());
      PROFILER_MARKER_TEXT(
          "SocketTransportService::Poll", NETWORK,
          MarkerTiming::IntervalUntilNowFrom(startTime),
          pollTimeout < 0
              ? nsPrintfCString("Poll count: %u, Poll timeout: NO_TIMEOUT",
                                pollCount)
          : pollTimeout == 0
              ? nsPrintfCString("Poll count: %u, Poll timeout: NO_WAIT",
                                pollCount)
              : nsPrintfCString("Poll count: %u, Poll timeout: %" PRId64 "ms",
                                pollCount, pollTimeout));
    }
#endif
  }

  SOCKET_LOG(("    ...returned after %i milliseconds\n",
              PR_IntervalToMilliseconds(PR_IntervalNow() - ts)));

  return n;
}

// Calls the PRFileDesc layer's poll methods separately for read and write
// interests. Layers may translate read requests to write operations (e.g.,
// during a TLS handshake), so split calls are needed to capture each
// mapping independently.
void nsSocketTransportService::PollLayersAndRegister(SocketContext* aContext,
                                                     int16_t aPollFlags,
                                                     int16_t* aOutFlags) {
  int16_t poll_read = 0, poll_write = 0, out_read = 0, out_write = 0;
  PRFileDesc* fd = aContext->mFD;
  if (aPollFlags & PR_POLL_READ) {
    poll_read = fd->methods->poll(
        fd, static_cast<int16_t>(aPollFlags & ~PR_POLL_WRITE), &out_read);
  }
  if (aPollFlags & PR_POLL_WRITE) {
    poll_write = fd->methods->poll(
        fd, static_cast<int16_t>(aPollFlags & ~PR_POLL_READ), &out_write);
  }
  if (aOutFlags) {
    *aOutFlags = static_cast<int16_t>(out_read | out_write);
  }

  // Skip the necko_poll_register FFI call if the layer poll results are
  // identical to the previous iteration. The Rust side diffs the computed
  // interest (readable/writable), but this check avoids the FFI crossing
  // and HashMap lookup entirely for the common unchanged case.
  if (aContext->mIsRegisteredWithPoller &&
      aPollFlags == aContext->mLastPollFlags &&
      poll_read == aContext->mLastPollFlagsForRead &&
      poll_write == aContext->mLastPollFlagsForWrite) {
    return;
  }

  aContext->mLastPollFlags = aPollFlags;
  aContext->mLastPollFlagsForRead = poll_read;
  aContext->mLastPollFlagsForWrite = poll_write;

  bool wantsRead = (aPollFlags & PR_POLL_READ) != 0;
  bool wantsWrite = (aPollFlags & PR_POLL_WRITE) != 0;
  bool wantsExcept = (aPollFlags & PR_POLL_EXCEPT) != 0;

  uintptr_t key = reinterpret_cast<uintptr_t>(aContext);

  [[maybe_unused]] PollResult result =
      necko_poll_register(mPoller.get(), aContext->mNativeFD, wantsRead,
                          wantsWrite, wantsExcept, poll_read, poll_write, key);
  MOZ_ASSERT(result == PollResult::Ok, "necko_poll_register failed");
  aContext->mIsRegisteredWithPoller = true;
}

//-----------------------------------------------------------------------------
// xpcom api

NS_IMPL_ISUPPORTS(nsSocketTransportService, nsISocketTransportService,
                  nsIRoutedSocketTransportService, nsIEventTarget,
                  nsISerialEventTarget, nsIThreadObserver, nsIRunnable,
                  nsPISocketTransportService, nsIObserver, nsINamed,
                  nsIDirectTaskDispatcher)

static const char* gCallbackPrefs[] = {
    SEND_BUFFER_PREF,
    KEEPALIVE_ENABLED_PREF,
    KEEPALIVE_IDLE_TIME_PREF,
    KEEPALIVE_RETRY_INTERVAL_PREF,
    KEEPALIVE_PROBE_COUNT_PREF,
    MAX_TIME_BETWEEN_TWO_POLLS,
    MAX_TIME_FOR_PR_CLOSE_DURING_SHUTDOWN,
    "network.socket.forcePort",
    nullptr,
};

/* static */
void nsSocketTransportService::UpdatePrefs(const char* aPref, void* aSelf) {
  static_cast<nsSocketTransportService*>(aSelf)->UpdatePrefs();
}

static uint32_t GetThreadStackSize() {
#ifdef XP_WIN
  if (!StaticPrefs::network_allow_large_stack_size_for_socket_thread()) {
    return nsIThreadManager::DEFAULT_STACK_SIZE;
  }

  const uint32_t kWindowsThreadStackSize = 512 * 1024;
  // We can remove this custom stack size when DEFAULT_STACK_SIZE is increased.
  static_assert(kWindowsThreadStackSize > nsIThreadManager::DEFAULT_STACK_SIZE);
  return kWindowsThreadStackSize;
#else
  return nsIThreadManager::DEFAULT_STACK_SIZE;
#endif
}

// called from main thread only
NS_IMETHODIMP
nsSocketTransportService::Init() {
  if (!NS_IsMainThread()) {
    NS_ERROR("wrong thread");
    return NS_ERROR_UNEXPECTED;
  }

  if (mInitialized) {
    return NS_OK;
  }

  if (mShuttingDown) {
    return NS_ERROR_UNEXPECTED;
  }

  nsCOMPtr<nsIThread> thread;

  if (!XRE_IsContentProcess() ||
      StaticPrefs::network_allow_raw_sockets_in_content_processes_AtStartup()) {
    // Since we Poll, we can't use normal LongTask support in Main Process
    nsresult rv = NS_NewNamedThread(
        "Socket Thread", getter_AddRefs(thread), this,
        {GetThreadStackSize(), false, false, Some(SOCKET_THREAD_LONGTASK_MS)});
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    // In the child process, we just want a regular nsThread with no socket
    // polling. So we don't want to run the nsSocketTransportService runnable on
    // it.
    nsresult rv =
        NS_NewNamedThread("Socket Thread", getter_AddRefs(thread), nullptr,
                          {nsIThreadManager::DEFAULT_STACK_SIZE, false, false,
                           Some(SOCKET_THREAD_LONGTASK_MS)});
    NS_ENSURE_SUCCESS(rv, rv);

    // Set up some of the state that nsSocketTransportService::Run would set.
    PRThread* prthread = nullptr;
    thread->GetPRThread(&prthread);
    gSocketThread = prthread;
    mRawThread = thread;
  }

  {
    MutexAutoLock lock(mLock);
    // Install our mThread, protecting against concurrent readers
    thread.swap(mThread);
    mDirectTaskDispatcher = do_QueryInterface(mThread);
    MOZ_DIAGNOSTIC_ASSERT(
        mDirectTaskDispatcher,
        "Underlying thread must support direct task dispatching");
  }

  Preferences::RegisterCallbacks(UpdatePrefs, gCallbackPrefs, this);
  UpdatePrefs();

  nsCOMPtr<nsIObserverService> obsSvc = services::GetObserverService();
  // Note that the observr notifications are forwarded from parent process to
  // socket process. We have to make sure the topics registered below are also
  // registered in nsIOService::Init().
  if (obsSvc) {
    MOZ_ALWAYS_SUCCEEDS(
        obsSvc->AddObserver(this, "last-pb-context-exited", false));
    MOZ_ALWAYS_SUCCEEDS(
        obsSvc->AddObserver(this, NS_WIDGET_SLEEP_OBSERVER_TOPIC, false));
    MOZ_ALWAYS_SUCCEEDS(
        obsSvc->AddObserver(this, NS_WIDGET_WAKE_OBSERVER_TOPIC, false));
    MOZ_ALWAYS_SUCCEEDS(
        obsSvc->AddObserver(this, "xpcom-shutdown-threads", false));
    MOZ_ALWAYS_SUCCEEDS(
        obsSvc->AddObserver(this, NS_NETWORK_LINK_TOPIC, false));
  }

  // We can now dispatch tasks to the socket thread.
  mInitialized = true;
  return NS_OK;
}

// called from main thread only
NS_IMETHODIMP
nsSocketTransportService::Shutdown(bool aXpcomShutdown) {
  SOCKET_LOG(("nsSocketTransportService::Shutdown\n"));

  NS_ENSURE_STATE(NS_IsMainThread());

  if (!mInitialized || mShuttingDown) {
    // We never inited, or shutdown has already started
    return NS_OK;
  }

  {
    auto observersCopy = mShutdownObservers;
    for (auto& observer : observersCopy) {
      observer->Observe();
    }
  }

  mShuttingDown = true;

  necko_poll_notify(mPoller.get());

  // If we're shutting down due to going offline (rather than due to XPCOM
  // shutdown), also tear down the thread. The thread will be shutdown during
  // xpcom-shutdown-threads if during xpcom-shutdown proper.
  if (!aXpcomShutdown) {
    ShutdownThread();
  }

  return NS_OK;
}

nsresult nsSocketTransportService::ShutdownThread() {
  SOCKET_LOG(("nsSocketTransportService::ShutdownThread\n"));

  NS_ENSURE_STATE(NS_IsMainThread());

  if (!mInitialized) {
    return NS_OK;
  }

  // join with thread
  nsCOMPtr<nsIThread> thread = GetThreadSafely();
  thread->Shutdown();
  {
    MutexAutoLock lock(mLock);
    // Drop our reference to mThread and make sure that any concurrent readers
    // are excluded
    mThread = nullptr;
    mDirectTaskDispatcher = nullptr;
  }

  Preferences::UnregisterCallbacks(UpdatePrefs, gCallbackPrefs, this);

  nsCOMPtr<nsIObserverService> obsSvc = services::GetObserverService();
  if (obsSvc) {
    obsSvc->RemoveObserver(this, "last-pb-context-exited");
    obsSvc->RemoveObserver(this, NS_WIDGET_SLEEP_OBSERVER_TOPIC);
    obsSvc->RemoveObserver(this, NS_WIDGET_WAKE_OBSERVER_TOPIC);
    obsSvc->RemoveObserver(this, "xpcom-shutdown-threads");
    obsSvc->RemoveObserver(this, NS_NETWORK_LINK_TOPIC);
  }

  if (mAfterWakeUpTimer) {
    mAfterWakeUpTimer->Cancel();
    mAfterWakeUpTimer = nullptr;
  }

  mInitialized = false;
  mShuttingDown = false;

  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::GetOffline(bool* offline) {
  MutexAutoLock lock(mLock);
  *offline = mOffline;
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::SetOffline(bool offline) {
  MutexAutoLock lock(mLock);
  if (!mOffline && offline) {
    // signal the socket thread to go offline, so it will detach sockets
    mGoingOffline = true;
    mOffline = true;
  } else if (mOffline && !offline) {
    mOffline = false;
  }

  necko_poll_notify(mPoller.get());
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::GetKeepaliveIdleTime(int32_t* aKeepaliveIdleTimeS) {
  MOZ_ASSERT(aKeepaliveIdleTimeS);
  if (NS_WARN_IF(!aKeepaliveIdleTimeS)) {
    return NS_ERROR_NULL_POINTER;
  }
  *aKeepaliveIdleTimeS = mKeepaliveIdleTimeS;
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::GetKeepaliveRetryInterval(
    int32_t* aKeepaliveRetryIntervalS) {
  MOZ_ASSERT(aKeepaliveRetryIntervalS);
  if (NS_WARN_IF(!aKeepaliveRetryIntervalS)) {
    return NS_ERROR_NULL_POINTER;
  }
  *aKeepaliveRetryIntervalS = mKeepaliveRetryIntervalS;
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::GetKeepaliveProbeCount(
    int32_t* aKeepaliveProbeCount) {
  MOZ_ASSERT(aKeepaliveProbeCount);
  if (NS_WARN_IF(!aKeepaliveProbeCount)) {
    return NS_ERROR_NULL_POINTER;
  }
  *aKeepaliveProbeCount = mKeepaliveProbeCount;
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::CreateTransport(const nsTArray<nsCString>& types,
                                          const nsACString& host, int32_t port,
                                          nsIProxyInfo* proxyInfo,
                                          nsIDNSRecord* dnsRecord,
                                          nsISocketTransport** result) {
  return CreateRoutedTransport(types, host, port, ""_ns, 0, proxyInfo,
                               dnsRecord, result);
}

NS_IMETHODIMP
nsSocketTransportService::CreateRoutedTransport(
    const nsTArray<nsCString>& types, const nsACString& host, int32_t port,
    const nsACString& hostRoute, int32_t portRoute, nsIProxyInfo* proxyInfo,
    nsIDNSRecord* dnsRecord, nsISocketTransport** result) {
  NS_ENSURE_TRUE(mInitialized, NS_ERROR_NOT_INITIALIZED);
  NS_ENSURE_TRUE(port >= 0 && port <= 0xFFFF, NS_ERROR_ILLEGAL_VALUE);

  RefPtr<nsSocketTransport> trans = new nsSocketTransport();
  nsresult rv = trans->Init(types, host, port, hostRoute, portRoute, proxyInfo,
                            dnsRecord);
  if (NS_FAILED(rv)) {
    return rv;
  }

  trans.forget(result);
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::CreateUnixDomainTransport(
    nsIFile* aPath, nsISocketTransport** result) {
#ifdef XP_UNIX
  nsresult rv;

  NS_ENSURE_TRUE(mInitialized, NS_ERROR_NOT_INITIALIZED);

  nsAutoCString path;
  rv = aPath->GetNativePath(path);
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<nsSocketTransport> trans = new nsSocketTransport();

  rv = trans->InitWithFilename(path.get());
  NS_ENSURE_SUCCESS(rv, rv);

  trans.forget(result);
  return NS_OK;
#else
  return NS_ERROR_SOCKET_ADDRESS_NOT_SUPPORTED;
#endif
}

NS_IMETHODIMP
nsSocketTransportService::CreateUnixDomainAbstractAddressTransport(
    const nsACString& aName, nsISocketTransport** result) {
  // Abstract socket address is supported on Linux only
#ifdef XP_LINUX
  RefPtr<nsSocketTransport> trans = new nsSocketTransport();
  // First character of Abstract socket address is null
  UniquePtr<char[]> name(new char[aName.Length() + 1]);
  *(name.get()) = 0;
  memcpy(name.get() + 1, aName.BeginReading(), aName.Length());
  nsresult rv = trans->InitWithName(name.get(), aName.Length() + 1);
  if (NS_FAILED(rv)) {
    return rv;
  }

  trans.forget(result);
  return NS_OK;
#else
  return NS_ERROR_SOCKET_ADDRESS_NOT_SUPPORTED;
#endif
}

NS_IMETHODIMP
nsSocketTransportService::OnDispatchedEvent() {
  // If already on the socket thread, no wakeup needed.
  if (OnSocketThread()) {
    return NS_OK;
  }
  necko_poll_notify(mPoller.get());
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::OnProcessNextEvent(nsIThreadInternal* thread,
                                             bool mayWait) {
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::AfterProcessNextEvent(nsIThreadInternal* thread,
                                                bool eventWasProcessed) {
  return NS_OK;
}

void nsSocketTransportService::MarkTheLastElementOfPendingQueue() {
  mServingPendingQueue = false;
}

NS_IMETHODIMP
nsSocketTransportService::Run() {
  SOCKET_LOG(("STS thread init %d sockets\n", gMaxCount));

#if defined(XP_WIN)
  // see bug 1361495, gethostname() triggers winsock initialization.
  // so do it here (on parent and child) to protect against it being done first
  // accidentally on the main thread.. especially via PR_GetSystemInfo(). This
  // will also improve latency of first real winsock operation
  // ..
  // If STS-thread is no longer needed this should still be run before exiting

  char ignoredStackBuffer[255];
  (void)gethostname(ignoredStackBuffer, 255);
#endif

  psm::InitializeSSLServerCertVerificationThreads();

  gSocketThread = PR_GetCurrentThread();
  mRawThread = NS_GetCurrentThread();

  // Ensure a call to GetCurrentSerialEventTarget() returns this event target.
  SerialEventTargetGuard guard(this);

  // hook ourselves up to observe event processing for this thread
  nsCOMPtr<nsIThreadInternal> threadInt = do_QueryInterface(mRawThread);
  threadInt->SetObserver(this);

  // make sure the pseudo random number generator is seeded on this thread
  srand(static_cast<unsigned>(PR_Now()));

  // For the calculation of the duration of the last cycle (i.e. the last
  // for-loop iteration before shutdown).
  TimeStamp startOfCycleForLastCycleCalc;

  // For measuring of the poll iteration duration without time spent blocked
  // in poll().
  TimeStamp pollCycleStart;

  // For calculating the time needed for a new element to run.
  TimeStamp startOfIteration;
  TimeStamp startOfNextIteration;

  for (;;) {
    bool pendingEvents = false;
    if (Telemetry::CanRecordPrereleaseData()) {
      startOfCycleForLastCycleCalc = TimeStamp::NowLoRes();
      startOfNextIteration = TimeStamp::NowLoRes();
    }
    // We pop out to this loop when there are no pending events.
    // If we don't reset these, we may not re-enter ProcessNextEvent()
    // until we have events to process, and it may seem like we have
    // an event running for a very long time.
    mRawThread->SetRunningEventDelay(TimeDuration(), TimeStamp());

    do {
      if (Telemetry::CanRecordPrereleaseData()) {
        pollCycleStart = TimeStamp::NowLoRes();
      }

      DoPollIteration();

      bool hadPriorityEvent = false;
      if (StaticPrefs::network_socket_prioritize_runnables()) {
        Queue<RefPtr<nsIRunnable>> queue;
        {
          AutoWriteLock lock(mQueueLock);
          queue = std::move(mPriorityEventQueue);
        }

        while (!queue.IsEmpty()) {
          RefPtr<nsIRunnable> event = queue.Pop();
          hadPriorityEvent = true;
          event->Run();
        }
      }

      mRawThread->HasPendingEvents(&pendingEvents);
      if (!hadPriorityEvent && pendingEvents) {
        if (!mServingPendingQueue) {
          nsresult rv = Dispatch(
              NewRunnableMethod(
                  "net::nsSocketTransportService::"
                  "MarkTheLastElementOfPendingQueue",
                  this,
                  &nsSocketTransportService::MarkTheLastElementOfPendingQueue),
              nsIEventTarget::DISPATCH_NORMAL);
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "Could not dispatch a new event on the "
                "socket thread.");
          } else {
            mServingPendingQueue = true;
          }

          if (Telemetry::CanRecordPrereleaseData()) {
            startOfIteration = startOfNextIteration;
            // Everything that comes after this point will
            // be served in the next iteration. If no even
            // arrives, startOfNextIteration will be reset at the
            // beginning of each for-loop.
            startOfNextIteration = TimeStamp::NowLoRes();
          }
        }
        TimeStamp eventQueueStart = TimeStamp::NowLoRes();
        do {
          NS_ProcessNextEvent(mRawThread);
          pendingEvents = false;
          mRawThread->HasPendingEvents(&pendingEvents);
        } while (pendingEvents && mServingPendingQueue &&
                 ((TimeStamp::NowLoRes() - eventQueueStart).ToMilliseconds() <
                  mMaxTimePerPollIter));
      }
      AutoReadLock lock(mQueueLock);
      pendingEvents = pendingEvents || !mPriorityEventQueue.IsEmpty();
    } while (pendingEvents);

    necko_poll_consume_notified(mPoller.get());

    bool goingOffline = false;
    // now that our event queue is empty, check to see if we should exit
    if (mShuttingDown) {
      break;
    }
    {
      MutexAutoLock lock(mLock);
      if (mGoingOffline) {
        mGoingOffline = false;
        goingOffline = true;
      }
    }
    // Avoid potential deadlock
    if (goingOffline) {
      Reset(true);
    }
  }

  SOCKET_LOG(("STS shutting down thread\n"));

  // detach all sockets, including locals
  Reset(false);

  // We don't clear gSocketThread so that OnSocketThread() won't be a false
  // alarm for events generated by stopping the SSL threads during shutdown.
  psm::StopSSLServerCertVerificationThreads();

  // Drain the priority event queue before final event processing
  {
    Queue<RefPtr<nsIRunnable>> queue;
    {
      AutoWriteLock lock(mQueueLock);
      queue = std::move(mPriorityEventQueue);
    }

    while (!queue.IsEmpty()) {
      RefPtr<nsIRunnable> event = queue.Pop();
      event->Run();
    }
  }

  // Final pass over the event queue. This makes sure that events posted by
  // socket detach handlers get processed.
  NS_ProcessPendingEvents(mRawThread);

  SOCKET_LOG(("STS thread exit\n"));
  MOZ_ASSERT(necko_poll_len(mPoller.get()) == 0);
  MOZ_ASSERT(mActiveList.IsEmpty());
  MOZ_ASSERT(mIdleList.IsEmpty());

  return NS_OK;
}

void nsSocketTransportService::DetachSocketWithGuard(
    bool aGuardLocals, SocketContextList& socketList, int32_t index) {
  bool isGuarded = false;
  if (aGuardLocals) {
    socketList[index].mHandler->IsLocal(&isGuarded);
    if (!isGuarded) {
      socketList[index].mHandler->KeepWhenOffline(&isGuarded);
    }
  }
  if (!isGuarded) {
    DetachSocket(socketList, &socketList[index]);
  }
}

void nsSocketTransportService::Reset(bool aGuardLocals) {
  // detach any sockets
  int32_t i;
  for (i = mActiveList.Length() - 1; i >= 0; --i) {
    DetachSocketWithGuard(aGuardLocals, mActiveList, i);
  }
  for (i = mIdleList.Length() - 1; i >= 0; --i) {
    DetachSocketWithGuard(aGuardLocals, mIdleList, i);
  }
}

nsresult nsSocketTransportService::DoPollIteration() {
  SOCKET_LOG(("STS poll iter\n"));

  PRIntervalTime now = PR_IntervalNow();

  // We can't have more than int32_max sockets in use
  int32_t i, count;
  //
  // poll loop
  //
  // walk active list backwards to see if any sockets should actually be
  // idle, then walk the idle list backwards to see if any idle sockets
  // should become active.  take care to check only idle sockets that
  // were idle to begin with ;-)
  //
  count = mIdleList.Length();

  PRIntervalTime minTimeout = NS_SOCKET_POLL_TIMEOUT;
  bool hasLayerReady = false;
  for (i = mActiveList.Length() - 1; i >= 0; --i) {
    //---
    SOCKET_LOG(("  active [%u] { handler=%p condition=%" PRIx32
                " pollflags=%hu }\n",
                i, mActiveList[i].mHandler.get(),
                static_cast<uint32_t>(mActiveList[i].mHandler->mCondition),
                mActiveList[i].mHandler->mPollFlags));
    //---
    if (NS_FAILED(mActiveList[i].mHandler->mCondition)) {
      DetachSocket(mActiveList, &mActiveList[i]);
    } else {
      int16_t in_flags =
          static_cast<int16_t>(mActiveList[i].mHandler->mPollFlags);
      if (in_flags == 0) {
        MoveToIdleList(&mActiveList[i]);
      } else {
        SocketContext& sock = mActiveList[i];
        int16_t out_flags = 0;
        PollLayersAndRegister(&sock, in_flags, &out_flags);

        if (out_flags != 0) {
          sock.mLayerReady = true;
          sock.mLayerOutFlags = out_flags;
          hasLayerReady = true;
          minTimeout = PR_INTERVAL_NO_WAIT;
        } else {
          sock.mLayerReady = false;
          sock.mLayerOutFlags = 0;
          sock.EnsureTimeout(now);
          PRIntervalTime t = sock.TimeoutIn(now);
          if (t < minTimeout) {
            minTimeout = t;
          }
        }
      }
    }
  }
  for (i = count - 1; i >= 0; --i) {
    //---
    SOCKET_LOG(("  idle [%u] { handler=%p condition=%" PRIx32
                " pollflags=%hu }\n",
                i, mIdleList[i].mHandler.get(),
                static_cast<uint32_t>(mIdleList[i].mHandler->mCondition),
                mIdleList[i].mHandler->mPollFlags));
    //---
    if (NS_FAILED(mIdleList[i].mHandler->mCondition)) {
      DetachSocket(mIdleList, &mIdleList[i]);
    } else if (mIdleList[i].mHandler->mPollFlags != 0) {
      // Compute the newly activated socket's timeout before MoveToPollList
      // transfers the handler. EnsureTimeout(now) inside AddToPollList will
      // set mPollStartEpoch = now, so the full timeout applies.
      uint16_t socketTimeout = mIdleList[i].mHandler->mPollTimeout;
      MoveToPollList(&mIdleList[i]);
      if (socketTimeout != UINT16_MAX) {
        PRIntervalTime t = PR_SecondsToInterval(socketTimeout);
        if (t < minTimeout) {
          minTimeout = t;
        }
      }
    }
  }

  if (minTimeout != NS_SOCKET_POLL_TIMEOUT) {
    SOCKET_LOG(
        ("poll timeout: %" PRIu32 "\n", PR_IntervalToSeconds(minTimeout)));
  }

  int32_t n = 0;
  bool skipPoll = gIOService->IsNetTearingDown();

  if (!skipPoll) {
    if (hasLayerReady) {
      // Layer data is buffered — do a non-blocking poll to also discover
      // OS-level events without blocking.
      n = Poll(now, PR_INTERVAL_NO_WAIT);
    } else {
      n = Poll(now, minTimeout);
    }
  } else {
    mPolledEvents.Clear();
    necko_poll_consume_notified(mPoller.get());
  }

  now = PR_IntervalNow();
#ifdef MOZ_GECKO_PROFILER
  TimeStamp startTime;
  bool profiling = profiler_thread_is_being_profiled_for_markers();
  if (profiling) {
    startTime = TimeStamp::Now();
  }
#endif

  if (n < 0) {
    SOCKET_LOG(
        ("  poll error [%d] os error [%d]\n", PR_GetError(), PR_GetOSError()));
  } else {
    //
    // service "active" sockets...
    //
    // First pass: scatter poll events into per-socket flags so the second
    // pass can handle events, timeouts, and layer-ready uniformly in a
    // single forward scan of mActiveList.
    for (const PollEvent& event : mPolledEvents) {
      auto* s = reinterpret_cast<SocketContext*>(event.key);
      s->mPollOutFlags = event.flags;
    }

    // Second pass: service sockets in forward order so earlier-added sockets
    // (typically the main document connection) are serviced first.
    for (i = 0; i < int32_t(mActiveList.Length()); ++i) {
      SocketContext& s = mActiveList[i];

      int16_t outFlags = 0;
      if (n > 0 && s.mPollOutFlags != 0) {
        outFlags = s.mPollOutFlags;
        s.mPollOutFlags = 0;
      } else if (s.IsTimedOut(now)) {
        SOCKET_LOG(("socket %p timed out", s.mHandler.get()));
        outFlags = -1;
      } else {
        s.MaybeResetEpoch();
        // Check for layer-ready sockets (e.g., buffered TLS data)
        if (s.mLayerReady && !NS_FAILED(s.mHandler->mCondition)) {
          outFlags = s.mLayerOutFlags;
          s.mLayerOutFlags = 0;
        }
      }

      if (outFlags != 0) {
        s.DisengageTimeout();
        s.mLayerReady = false;
        s.mHandler->OnSocketReady(s.mFD, outFlags);
      }
    }

    // Third pass: clean up dead sockets in reverse order so swap-removal
    // doesn't affect unvisited elements.
    for (i = mActiveList.Length() - 1; i >= 0; --i) {
      if (NS_FAILED(mActiveList[i].mHandler->mCondition)) {
        DetachSocket(mActiveList, &mActiveList[i]);
      }
    }
  }
#ifdef MOZ_GECKO_PROFILER
  if (profiling) {
    TimeStamp endTime = TimeStamp::Now();
    if ((endTime - startTime).ToMilliseconds() >= SOCKET_THREAD_LONGTASK_MS) {
      struct LongTaskMarker {
        static constexpr Span<const char> MarkerTypeName() {
          return MakeStringSpan("SocketThreadLongTask");
        }
        static void StreamJSONMarkerData(
            baseprofiler::SpliceableJSONWriter& aWriter) {
          aWriter.StringProperty("category", "LongTask");
        }
        static MarkerSchema MarkerTypeDisplay() {
          using MS = MarkerSchema;
          MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
          schema.AddKeyLabelFormat("category", "Type", MS::Format::String);
          return schema;
        }
      };

      profiler_add_marker(ProfilerString8View("LongTaskSocketProcessing"),
                          geckoprofiler::category::OTHER,
                          MarkerTiming::Interval(startTime, endTime),
                          LongTaskMarker{});
    }
  }

#endif

  return NS_OK;
}

void nsSocketTransportService::UpdateSendBufferPref() {
  int32_t bufferSize;

  // If the pref is set, honor it. 0 means use OS defaults.
  nsresult rv = Preferences::GetInt(SEND_BUFFER_PREF, &bufferSize);
  if (NS_SUCCEEDED(rv)) {
    mSendBufferSize = bufferSize;
    return;
  }

#if defined(XP_WIN)
  mSendBufferSize = 131072 * 4;
#endif
}

nsresult nsSocketTransportService::UpdatePrefs() {
  mSendBufferSize = 0;

  UpdateSendBufferPref();

  // Default TCP Keepalive Values.
  int32_t keepaliveIdleTimeS;
  nsresult rv =
      Preferences::GetInt(KEEPALIVE_IDLE_TIME_PREF, &keepaliveIdleTimeS);
  if (NS_SUCCEEDED(rv)) {
    mKeepaliveIdleTimeS = std::clamp(keepaliveIdleTimeS, 1, kMaxTCPKeepIdle);
  }

  int32_t keepaliveRetryIntervalS;
  rv = Preferences::GetInt(KEEPALIVE_RETRY_INTERVAL_PREF,
                           &keepaliveRetryIntervalS);
  if (NS_SUCCEEDED(rv)) {
    mKeepaliveRetryIntervalS =
        std::clamp(keepaliveRetryIntervalS, 1, kMaxTCPKeepIntvl);
  }

  int32_t keepaliveProbeCount;
  rv = Preferences::GetInt(KEEPALIVE_PROBE_COUNT_PREF, &keepaliveProbeCount);
  if (NS_SUCCEEDED(rv)) {
    mKeepaliveProbeCount = std::clamp(keepaliveProbeCount, 1, kMaxTCPKeepCount);
  }
  bool keepaliveEnabled = false;
  rv = Preferences::GetBool(KEEPALIVE_ENABLED_PREF, &keepaliveEnabled);
  if (NS_SUCCEEDED(rv) && keepaliveEnabled != mKeepaliveEnabledPref) {
    mKeepaliveEnabledPref = keepaliveEnabled;
    OnKeepaliveEnabledPrefChange();
  }

  int32_t maxTimePref;
  rv = Preferences::GetInt(MAX_TIME_BETWEEN_TWO_POLLS, &maxTimePref);
  if (NS_SUCCEEDED(rv) && maxTimePref >= 0) {
    mMaxTimePerPollIter = maxTimePref;
  }

  int32_t pollBusyWaitPeriod;
  rv = Preferences::GetInt(POLL_BUSY_WAIT_PERIOD, &pollBusyWaitPeriod);
  if (NS_SUCCEEDED(rv) && pollBusyWaitPeriod > 0) {
    mNetworkLinkChangeBusyWaitPeriod = PR_SecondsToInterval(pollBusyWaitPeriod);
  }

  int32_t pollBusyWaitPeriodTimeout;
  rv = Preferences::GetInt(POLL_BUSY_WAIT_PERIOD_TIMEOUT,
                           &pollBusyWaitPeriodTimeout);
  if (NS_SUCCEEDED(rv) && pollBusyWaitPeriodTimeout > 0) {
    mNetworkLinkChangeBusyWaitTimeout =
        PR_SecondsToInterval(pollBusyWaitPeriodTimeout);
  }

  int32_t maxTimeForPrClosePref;
  rv = Preferences::GetInt(MAX_TIME_FOR_PR_CLOSE_DURING_SHUTDOWN,
                           &maxTimeForPrClosePref);
  if (NS_SUCCEEDED(rv) && maxTimeForPrClosePref >= 0) {
    mMaxTimeForPrClosePref = PR_MillisecondsToInterval(maxTimeForPrClosePref);
  }

  nsAutoCString portMappingPref;
  rv = Preferences::GetCString("network.socket.forcePort", portMappingPref);
  if (NS_SUCCEEDED(rv)) {
    bool rv = UpdatePortRemapPreference(portMappingPref);
    if (!rv) {
      NS_ERROR(
          "network.socket.forcePort preference is ill-formed, this will likely "
          "make everything unexpectedly fail!");
    }
  }

  return NS_OK;
}

void nsSocketTransportService::OnKeepaliveEnabledPrefChange() {
  // Dispatch to socket thread if we're not executing there.
  if (!OnSocketThread()) {
    gSocketTransportService->Dispatch(
        NewRunnableMethod(
            "net::nsSocketTransportService::OnKeepaliveEnabledPrefChange", this,
            &nsSocketTransportService::OnKeepaliveEnabledPrefChange),
        NS_DISPATCH_NORMAL);
    return;
  }

  SOCKET_LOG(("nsSocketTransportService::OnKeepaliveEnabledPrefChange %s",
              mKeepaliveEnabledPref ? "enabled" : "disabled"));

  // Notify each socket that keepalive has been en/disabled globally.
  for (int32_t i = mActiveList.Length() - 1; i >= 0; --i) {
    NotifyKeepaliveEnabledPrefChange(&mActiveList[i]);
  }
  for (int32_t i = mIdleList.Length() - 1; i >= 0; --i) {
    NotifyKeepaliveEnabledPrefChange(&mIdleList[i]);
  }
}

void nsSocketTransportService::NotifyKeepaliveEnabledPrefChange(
    SocketContext* sock) {
  MOZ_ASSERT(sock, "SocketContext cannot be null!");
  MOZ_ASSERT(sock->mHandler, "SocketContext does not have a handler!");

  if (!sock || !sock->mHandler) {
    return;
  }

  sock->mHandler->OnKeepaliveEnabledPrefChange(mKeepaliveEnabledPref);
}

NS_IMETHODIMP
nsSocketTransportService::GetName(nsACString& aName) {
  aName.AssignLiteral("nsSocketTransportService");
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::Observe(nsISupports* subject, const char* topic,
                                  const char16_t* data) {
  SOCKET_LOG(("nsSocketTransportService::Observe topic=%s", topic));

  if (!strcmp(topic, "last-pb-context-exited")) {
    nsCOMPtr<nsIRunnable> ev = NewRunnableMethod(
        "net::nsSocketTransportService::ClosePrivateConnections", this,
        &nsSocketTransportService::ClosePrivateConnections);
    nsresult rv = Dispatch(ev, nsIEventTarget::DISPATCH_NORMAL);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (!strcmp(topic, NS_TIMER_CALLBACK_TOPIC)) {
    nsCOMPtr<nsITimer> timer = do_QueryInterface(subject);
    if (timer == mAfterWakeUpTimer) {
      mAfterWakeUpTimer = nullptr;
      mSleepPhase = false;
    }
  } else if (!strcmp(topic, NS_WIDGET_SLEEP_OBSERVER_TOPIC)) {
    mSleepPhase = true;
    if (mAfterWakeUpTimer) {
      mAfterWakeUpTimer->Cancel();
      mAfterWakeUpTimer = nullptr;
    }
  } else if (!strcmp(topic, NS_WIDGET_WAKE_OBSERVER_TOPIC)) {
    if (mSleepPhase && !mAfterWakeUpTimer) {
      NS_NewTimerWithObserver(getter_AddRefs(mAfterWakeUpTimer), this, 2000,
                              nsITimer::TYPE_ONE_SHOT);
    }
  } else if (!strcmp(topic, "xpcom-shutdown-threads")) {
    ShutdownThread();
  } else if (!strcmp(topic, NS_NETWORK_LINK_TOPIC)) {
    mLastNetworkLinkChangeTime = PR_IntervalNow();
  }

  return NS_OK;
}

void nsSocketTransportService::ClosePrivateConnections() {
  MOZ_ASSERT(IsOnCurrentThread(), "Must be called on the socket thread");

  for (int32_t i = mActiveList.Length() - 1; i >= 0; --i) {
    if (mActiveList[i].mHandler->mIsPrivate) {
      DetachSocket(mActiveList, &mActiveList[i]);
    }
  }
  for (int32_t i = mIdleList.Length() - 1; i >= 0; --i) {
    if (mIdleList[i].mHandler->mIsPrivate) {
      DetachSocket(mIdleList, &mIdleList[i]);
    }
  }
}

NS_IMETHODIMP
nsSocketTransportService::GetSendBufferSize(int32_t* value) {
  *value = mSendBufferSize;
  return NS_OK;
}

/// ugly OS specific includes are placed at the bottom of the src for clarity

#if defined(XP_WIN)
#  include <windows.h>
#elif defined(XP_UNIX) && !defined(AIX) && !defined(NEXTSTEP) && !defined(QNX)
#  include <sys/resource.h>
#endif

PRStatus nsSocketTransportService::DiscoverMaxCount() {
  gMaxCount = SOCKET_LIMIT_MIN;

#if defined(XP_UNIX) && !defined(AIX) && !defined(NEXTSTEP) && !defined(QNX)
  // On unix and os x network sockets and file
  // descriptors are the same. OS X comes defaulted at 256,
  // most linux at 1000. We can reliably use [sg]rlimit to
  // query that and raise it if needed.

  struct rlimit rlimitData{};
  if (getrlimit(RLIMIT_NOFILE, &rlimitData) == -1) {  // rlimit broken - use min
    return PR_SUCCESS;
  }

  if (rlimitData.rlim_cur >= SOCKET_LIMIT_TARGET) {  // larger than target!
    gMaxCount = SOCKET_LIMIT_TARGET;
    return PR_SUCCESS;
  }

  int32_t maxallowed = rlimitData.rlim_max;
  if ((uint32_t)maxallowed <= SOCKET_LIMIT_MIN) {
    return PR_SUCCESS;  // so small treat as if rlimit is broken
  }

  if ((maxallowed == -1) ||  // no hard cap - ok to set target
      ((uint32_t)maxallowed >= SOCKET_LIMIT_TARGET)) {
    maxallowed = SOCKET_LIMIT_TARGET;
  }

  rlimitData.rlim_cur = maxallowed;
  setrlimit(RLIMIT_NOFILE, &rlimitData);
  if ((getrlimit(RLIMIT_NOFILE, &rlimitData) != -1) &&
      (rlimitData.rlim_cur > SOCKET_LIMIT_MIN)) {
    if (rlimitData.rlim_cur > SOCKET_LIMIT_TARGET) {
      SOCKET_LOG(
          ("DiscoverMaxCount: rlim_cur=%llu exceeds SOCKET_LIMIT_TARGET, "
           "capping to %u",
           (unsigned long long)rlimitData.rlim_cur, SOCKET_LIMIT_TARGET));
    }
    // gMaxCount must not exceed SOCKET_LIMIT_TARGET because the constructor
    // pre-allocates mActiveList and mIdleList to that size.
    gMaxCount = std::min(static_cast<uint32_t>(rlimitData.rlim_cur),
                         SOCKET_LIMIT_TARGET);
  }

#elif defined(XP_WIN) && !defined(WIN_CE)
  // >= XP is confirmed to have at least 1000
  static_assert(SOCKET_LIMIT_TARGET <= 1000,
                "SOCKET_LIMIT_TARGET max value is 1000");
  gMaxCount = SOCKET_LIMIT_TARGET;
#else
  // other platforms are harder to test - so leave at safe legacy value
#endif

  return PR_SUCCESS;
}

// Used to return connection info to Dashboard.cpp
void nsSocketTransportService::AnalyzeConnection(nsTArray<SocketInfo>* data,
                                                 SocketContext* context,
                                                 bool aActive) {
  if (context->mHandler->mIsPrivate) {
    return;
  }
  PRFileDesc* aFD = context->mFD;

  PRFileDesc* idLayer = PR_GetIdentitiesLayer(aFD, PR_NSPR_IO_LAYER);

  NS_ENSURE_TRUE_VOID(idLayer);

  PRDescType type = PR_GetDescType(idLayer);
  char host[64] = {0};
  uint16_t port;
  const char* type_desc;

  if (type == PR_DESC_SOCKET_TCP) {
    type_desc = "TCP";
    PRNetAddr peer_addr;
    PodZero(&peer_addr);

    PRStatus rv = PR_GetPeerName(aFD, &peer_addr);
    if (rv != PR_SUCCESS) {
      return;
    }

    rv = PR_NetAddrToString(&peer_addr, host, sizeof(host));
    if (rv != PR_SUCCESS) {
      return;
    }

    if (peer_addr.raw.family == PR_AF_INET) {
      port = peer_addr.inet.port;
    } else {
      port = peer_addr.ipv6.port;
    }
    port = PR_ntohs(port);
  } else {
    if (type == PR_DESC_SOCKET_UDP) {
      type_desc = "UDP";
    } else {
      type_desc = "other";
    }
    NetAddr addr;
    if (context->mHandler->GetRemoteAddr(&addr) != NS_OK) {
      return;
    }
    if (!addr.ToStringBuffer(host, sizeof(host))) {
      return;
    }
    if (addr.GetPort(&port) != NS_OK) {
      return;
    }
  }

  uint64_t sent = context->mHandler->ByteCountSent();
  uint64_t received = context->mHandler->ByteCountReceived();
  SocketInfo info = {nsCString(host),     sent, received, port, aActive,
                     nsCString(type_desc)};

  data->AppendElement(info);
}

void nsSocketTransportService::GetSocketConnections(
    nsTArray<SocketInfo>* data) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  for (uint32_t i = 0; i < mActiveList.Length(); i++) {
    AnalyzeConnection(data, &mActiveList[i], true);
  }
  for (uint32_t i = 0; i < mIdleList.Length(); i++) {
    AnalyzeConnection(data, &mIdleList[i], false);
  }
}

bool nsSocketTransportService::IsTelemetryEnabledAndNotSleepPhase() {
  return Telemetry::CanRecordPrereleaseData() && !mSleepPhase;
}

NS_IMETHODIMP
nsSocketTransportService::AddShutdownObserver(
    nsISTSShutdownObserver* aObserver) {
  mShutdownObservers.AppendElement(aObserver);
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::RemoveShutdownObserver(
    nsISTSShutdownObserver* aObserver) {
  mShutdownObservers.RemoveElement(aObserver);
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::ChangeFileDescNativeHandleWithPoller(
    PRFileDesc* fd1, PRFileDesc* fd2) {
  MOZ_ASSERT(fd1 && fd2, "null file descriptor swap");

  // Get the native handles before swapping
  PROsfd osfd1 = PR_FileDesc2NativeHandle(fd1);
  PROsfd osfd2 = PR_FileDesc2NativeHandle(fd2);

  MOZ_ASSERT(osfd1 >= 0 && osfd2 >= 0, "invalid native file descriptor");

  // Check if the old file descriptor is registered with the poller
  uint16_t pollFlags = 0;
  SocketContext* foundSock = nullptr;

  // Linear scan by native fd. This is fine because fd swaps only happen
  // during TLS connection upgrades, which are rare and not on the hot path.
  for (auto& sock : mActiveList) {
    if (sock.mNativeFD == osfd1) {
      pollFlags = sock.mHandler->mPollFlags;
      foundSock = &sock;

      // Remove from poller before swapping handles
      MOZ_ASSERT(sock.mIsRegisteredWithPoller,
                 "socket should be registered with poller");
      [[maybe_unused]] PollResult result =
          necko_poll_delete(mPoller.get(), osfd1);
      MOZ_ASSERT(result == PollResult::Ok,
                 "necko_poll_delete failed for fd swap");
      sock.mIsRegisteredWithPoller = false;
      break;
    }
  }

  // Perform the handle swap
  PR_ChangeFileDescNativeHandle(fd1, osfd2);
  PR_ChangeFileDescNativeHandle(fd2, osfd1);

  // Re-register with the new handle if it was previously registered
  if (pollFlags && foundSock) {
    foundSock->mNativeFD = osfd2;
    PollLayersAndRegister(foundSock, static_cast<int16_t>(pollFlags), nullptr);
  }
  return NS_OK;
}

}  // namespace net
}  // namespace mozilla
