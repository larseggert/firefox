/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSocketTransportService2.h"

#include "mozilla/Atomics.h"
#include "mozilla/ChaosMode.h"
#include "mozilla/DebugOnly.h"
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
#include "mozilla/Telemetry.h"
#include "mozilla/Tokenizer.h"
#include "mozilla/glean/NetwerkMetrics.h"
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
#include "private/pprio.h"

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
#define POLLABLE_EVENT_TIMEOUT "network.sts.pollable_event_timeout"

#define REPAIR_POLLABLE_EVENT_TIME 10

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
    : mPollableEventTimeout(TimeDuration::FromSeconds(6)),
      mMaxTimeForPrClosePref(PR_SecondsToInterval(5)),
      mNetworkLinkChangeBusyWaitPeriod(PR_SecondsToInterval(50)),
      mNetworkLinkChangeBusyWaitTimeout(PR_SecondsToInterval(7)) {
  NS_ASSERTION(NS_IsMainThread(), "wrong thread");

  PR_CallOnce(&gMaxCountInitOnce, DiscoverMaxCount);

  NS_ASSERTION(!gSocketTransportService, "must not instantiate twice");
  gSocketTransportService = this;

  // The Poll list always has an entry at [0].   The rest of the
  // list is a duplicate of the Active list's PRFileDesc file descriptors.
  PRPollDesc entry = {nullptr, PR_POLL_READ | PR_POLL_EXCEPT, 0};
  mPollList.InsertElementAt(0, entry);
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
    // We need to call OnDispatchedEvent to ensure that mPollableEvent
    // gets signalled when an event is dispatched from another thread.
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

#ifdef XP_UNIX
#  ifdef XP_DARWIN
  // See the Darwin case in config/external/nspr/pr/moz.build
  static constexpr PROsfd kFDs = 4096;
#  else
  static constexpr PROsfd kFDs = 65536;
#  endif
  PROsfd osfd = PR_FileDesc2NativeHandle(fd);
  // If the native fd exceeds what PR_Poll can handle, PR_Poll will treat it as
  // invalid (POLLNVAL) and networking degrades into hard-to-debug failures.
  // Crash early with a clear reason instead. See bug 1980171 for context.
  MOZ_RELEASE_ASSERT(osfd < kFDs);
#endif

  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  if (!CanAttachSocket()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  SocketContext sock{fd, handler, 0};

  if (mPollerBackend) {
    // Registered exactly once for the socket's whole attached lifetime
    // (Remove() only in DetachSocket()); going idle and reactivating never
    // re-registers it (mActiveList holds every attached socket in this
    // mode -- there's no separate idle list to migrate into/out of at all),
    // so a stateful backend never pays kernel add/remove churn for a
    // socket that's merely quiescent.
    PollerFd nativeFd = PR_FileDesc2NativeHandle(fd);
    mPollerBackend->Add(nativeFd, 0, reinterpret_cast<void*>(nativeFd));
    // Avoid refcount bump/decrease
    mActiveList.EmplaceBack(sock.mFD, sock.mHandler.forget(),
                            sock.mPollStartEpoch);
  } else {
    AddToIdleList(&sock);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSocketTransportService::ChangeFileDescNativeHandleWithPoller(
    PRFileDesc* aFd1, PRFileDesc* aFd2) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  // nsSOCKSIOLayer's address-family fixup can run on a socket that is
  // already connecting/connected -- i.e. already in mActiveList and, when
  // mPollerBackend is set, already registered under its *current* native
  // fd. PR_ChangeFileDescNativeHandle() alone would desync that
  // registration from reality (it would go on polling the old, about-to-be
  // reused-or-closed fd). Capture the old native fd, do the swap, then
  // re-register the new one in its place if it was registered at all.
  PollerFd oldFd1 = PR_FileDesc2NativeHandle(aFd1);
  PollerFd oldFd2 = PR_FileDesc2NativeHandle(aFd2);

  // Symmetric: the IDL contract doesn't restrict aFd2 to being unregistered
  // (today's only caller happens to pass a fresh, unregistered fd here, but
  // nothing enforces that for future callers), so both sides are handled.
  // Every attached socket -- active or idle -- is registered with
  // mPollerBackend for its whole lifetime (see AttachSocket()), so asking
  // the backend's own Remove() whether it found something is simpler and
  // more trustworthy than re-deriving "is this registered" by scanning
  // mActiveList/mIdleList ourselves in a second, separately-maintained copy
  // of the same fact.
  bool fd1WasRegistered = false;
  bool fd2WasRegistered = false;
  if (mPollerBackend) {
    fd1WasRegistered = NS_SUCCEEDED(mPollerBackend->Remove(oldFd1));
    fd2WasRegistered = NS_SUCCEEDED(mPollerBackend->Remove(oldFd2));
  }

  PR_ChangeFileDescNativeHandle(aFd1, oldFd2);
  PR_ChangeFileDescNativeHandle(aFd2, oldFd1);

  // Placeholder interest; the next DoPollIterationWithBackend() walk
  // resolves and Modify()s the real value before any blocking Wait().
  if (fd1WasRegistered) {
    mPollerBackend->Add(oldFd2, 0, reinterpret_cast<void*>(oldFd2));
  }
  if (fd2WasRegistered) {
    mPollerBackend->Add(oldFd1, 0, reinterpret_cast<void*>(oldFd1));
  }
  return NS_OK;
}

// Converts an SSLReadiness to the kPoller{Read,Write} bits to register with
// mPollerBackend -- OR of both directions' OS-level interest, matching
// WalkSocketLayers()'s r.osInterest. Gecko-side translation of NSS's data,
// not an NSS concept -- kPollerRead/Write are this poller backend's own bit
// layout.
static int16_t PollerInterestFromReadiness(const SSLReadiness& aReadiness) {
  int16_t interest = 0;
  if (aReadiness.readWantsOsRead || aReadiness.writeWantsOsRead) {
    interest |= kPollerRead;
  }
  if (aReadiness.readWantsOsWrite || aReadiness.writeWantsOsWrite) {
    interest |= kPollerWrite;
  }
  return interest;
}

// Converts an SSLReadiness to the kPoll{Read,Write}Sys{Read,Write}
// direction map for UnmapReadyFlags(), matching WalkSocketLayers()'s
// r.directionMap. Gecko-side translation, not an NSS concept -- this
// encoding is local to Poller.cpp/UnmapReadyFlags().
static int16_t DirectionMapFromReadiness(const SSLReadiness& aReadiness) {
  int16_t directionMap = 0;
  if (aReadiness.readWantsOsRead) directionMap |= kPollReadSysRead;
  if (aReadiness.readWantsOsWrite) directionMap |= kPollReadSysWrite;
  if (aReadiness.writeWantsOsRead) directionMap |= kPollWriteSysRead;
  if (aReadiness.writeWantsOsWrite) directionMap |= kPollWriteSysWrite;
  return directionMap;
}

// Whether an SSLReadiness-managed fd belongs on mReadyNowList -- i.e. needs
// OnSocketReady() called this iteration despite reporting no OS-level
// interest, because either decrypted plaintext is already buffered, or the
// connection has permanently failed and nothing will ever arrive to ask for
// OS interest again. mReadyNowList is Gecko's own concept; this is not an
// NSS-defined predicate.
static bool ShouldJoinReadyNowList(const SSLReadiness& aReadiness) {
  return aReadiness.plaintextReady || aReadiness.terminallyFailed;
}

NS_IMETHODIMP
nsSocketTransportService::OnTLSReadinessChanged(
    PRFileDesc* aFd, const SSLReadiness& aReadiness) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  // NSSSocketControl only registers this callback when the same
  // network.sts.use_nspr_for_polling pref that decided mPollerBackend here
  // indicates the new backend, so this should never fire under the legacy
  // PR_Poll path; a mismatch means the two checks drifted out of sync.
  MOZ_DIAGNOSTIC_ASSERT(mPollerBackend,
                        "TLS readiness callback fired without a poller backend");
  if (!mPollerBackend) {
    return NS_OK;
  }

  PollerFd fd = PR_FileDesc2NativeHandle(aFd);
  int32_t idx = FindActiveIndexByNativeFD(fd);
  if (idx < 0) {
    // Not attached (yet, or anymore); nothing to update.
    return NS_OK;
  }
  SocketContext& sock = mActiveList[idx];

  // From here on DoPollIterationWithBackend() must not also
  // WalkSocketLayers()/Modify() this fd -- see mNSSReadinessManaged's
  // comment on SocketContext.
  sock.mNSSReadinessManaged = true;
  sock.mReadiness = aReadiness;

  int16_t interest = PollerInterestFromReadiness(aReadiness);
  DebugOnly<nsresult> rv = mPollerBackend->Modify(fd, interest);
  MOZ_ASSERT(NS_SUCCEEDED(rv),
             "NSS-readiness-managed socket not registered with mPollerBackend");

  // aReadiness.terminallyFailed fds report zero OS interest just like a
  // genuine async pause, but nothing will ever arrive to change that --
  // keep them on the ready-now list too so OnSocketReady() still gets
  // called and the failure can be discovered via the next read/write
  // attempt.
  bool wantsReadyNow = ShouldJoinReadyNowList(aReadiness);
  if (wantsReadyNow && !sock.mOnReadyNowList) {
    mReadyNowList.AppendElement(fd);
    sock.mOnReadyNowList = true;
  } else if (!wantsReadyNow && sock.mOnReadyNowList) {
    mReadyNowList.RemoveElement(fd);
    sock.mOnReadyNowList = false;
  }

  return NS_OK;
}

// the number of sockets that can be attached at any given time is
// limited.  this is done because some operating systems (e.g., Win9x)
// limit the number of sockets that can be created by an application.
// AttachSocket will fail if the limit is exceeded.  consumers should
// call CanAttachSocket and check the result before creating a socket.

bool nsSocketTransportService::CanAttachSocket() {
  MOZ_ASSERT(!mShuttingDown);
  // Correct in both polling modes: in backend mode mIdleList is always
  // empty (see AttachSocket()/DetachSocket()), so this is just
  // mActiveList's length there.
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
  MOZ_ASSERT(!mPollerBackend || &listHead == &mActiveList,
             "backend mode only ever attaches to mActiveList");

  // Capture the native fd before OnSocketDetached() below, which commonly
  // drops the last reference to sock->mFD and synchronously PR_Closes (and
  // thus frees) it -- reading sock->mFD afterwards would be a use-after-free.
  // mIdleList sockets are never backend-registered (mPollerBackend is only
  // ever non-null when every attached socket lives in mActiveList; see
  // AttachSocket()).
  PollerFd fd =
      mPollerBackend ? PR_FileDesc2NativeHandle(sock->mFD) : -1;

  if (fd != -1) {
    // Removed before OnSocketDetached() below, not after: that call
    // commonly PR_Closes the native fd synchronously, and some handlers
    // (e.g. nsUDPSocket with a sync listener) synchronously drive a
    // reentrant AttachSocket() out of that same call. If the OS hands the
    // just-freed fd number to that new socket before this Remove() ran,
    // PollerBackend::Add() would find it still registered and
    // MOZ_RELEASE_ASSERT. Removing first closes that window regardless of
    // what OnSocketDetached() does.
    mPollerBackend->Remove(fd);
    // Stale once detached -- NSS's OnTLSReadinessChanged() call that would
    // otherwise remove it (plaintextReady turning false) can never fire
    // again for this fd.
    mReadyNowList.RemoveElement(fd);
  }

  {
    // inform the handler that this socket is going away
    sock->mHandler->OnSocketDetached(sock->mFD);
  }
  mSentBytesCount += sock->mHandler->ByteCountSent();
  mReceivedBytesCount += sock->mHandler->ByteCountReceived();

  // cleanup
  sock->mFD = nullptr;

  if (mPollerBackend) {
    // mActiveList holds every attached socket in this mode; a plain
    // removal, no mPollList/RemoveFromPollList() involved (see
    // AddToPollList()'s TODO comment -- that legacy bookkeeping is never
    // reached here).
    auto index = SockIndex(mActiveList, sock);
    MOZ_RELEASE_ASSERT(index != -1, "invalid index");
    mActiveList.UnorderedRemoveElementAt(index);
  } else if (&listHead == &mActiveList) {
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

// TODO: remove once network.sts.use_nspr_for_polling is retired.
// Legacy PR_Poll path only -- never reached when mPollerBackend is set (see
// AttachSocket()/DetachSocket(), which append to/remove from mActiveList
// directly in that case).
void nsSocketTransportService::AddToPollList(SocketContext* sock) {
  MOZ_ASSERT(!mPollerBackend);
  MOZ_ASSERT(SockIndex(mActiveList, sock) == -1,
             "AddToPollList Socket Already Active");

  SOCKET_LOG(("nsSocketTransportService::AddToPollList %p [handler=%p]\n", sock,
              sock->mHandler.get()));

  sock->EnsureTimeout(PR_IntervalNow());

  PRPollDesc poll;
  poll.fd = sock->mFD;
  poll.in_flags = sock->mHandler->mPollFlags;
  poll.out_flags = 0;
  if (ChaosMode::isActive(ChaosFeature::NetworkScheduling)) {
    auto newSocketIndex = mActiveList.Length();
    newSocketIndex = ChaosMode::randomUint32LessThan(newSocketIndex + 1);
    mActiveList.InsertElementAt(newSocketIndex, *sock);
    // mPollList is offset by 1
    mPollList.InsertElementAt(newSocketIndex + 1, poll);
  } else {
    // Avoid refcount bump/decrease
    mActiveList.EmplaceBack(sock->mFD, sock->mHandler.forget(),
                            sock->mPollStartEpoch);
    mPollList.AppendElement(poll);
  }

  SOCKET_LOG(
      ("  active=%zu idle=%zu\n", mActiveList.Length(), mIdleList.Length()));
}

// TODO: remove once network.sts.use_nspr_for_polling is retired.
// Legacy PR_Poll path only -- never reached when mPollerBackend is set.
void nsSocketTransportService::RemoveFromPollList(SocketContext* sock) {
  MOZ_ASSERT(!mPollerBackend);
  SOCKET_LOG(("nsSocketTransportService::RemoveFromPollList %p [handler=%p]\n",
              sock, sock->mHandler.get()));

  auto index = SockIndex(mActiveList, sock);
  MOZ_RELEASE_ASSERT(index != -1, "invalid index");

  SOCKET_LOG(("  index=%" PRId64 " mActiveList.Length()=%zu\n", index,
              mActiveList.Length()));
  mActiveList.UnorderedRemoveElementAt(index);
  // mPollList is offset by 1
  mPollList.UnorderedRemoveElementAt(index + 1);

  SOCKET_LOG(
      ("  active=%zu idle=%zu\n", mActiveList.Length(), mIdleList.Length()));
}

// TODO: remove once network.sts.use_nspr_for_polling is retired.
// Legacy PR_Poll path only -- never reached when mPollerBackend is set (see
// AttachSocket(), which appends to mActiveList directly in that case).
void nsSocketTransportService::AddToIdleList(SocketContext* sock) {
  MOZ_ASSERT(!mPollerBackend);
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

// TODO: remove once network.sts.use_nspr_for_polling is retired.
// Legacy PR_Poll path only -- never reached when mPollerBackend is set.
void nsSocketTransportService::RemoveFromIdleList(SocketContext* sock) {
  MOZ_ASSERT(!mPollerBackend);
  SOCKET_LOG(("nsSocketTransportService::RemoveFromIdleList [handler=%p]\n",
              sock->mHandler.get()));
  auto index = SockIndex(mIdleList, sock);
  MOZ_RELEASE_ASSERT(index != -1);
  mIdleList.UnorderedRemoveElementAt(index);

  SOCKET_LOG(
      ("  active=%zu idle=%zu\n", mActiveList.Length(), mIdleList.Length()));
}

// TODO: remove once network.sts.use_nspr_for_polling is retired.
// Legacy PR_Poll path only -- never reached when mPollerBackend is set.
void nsSocketTransportService::MoveToIdleList(SocketContext* sock) {
  MOZ_ASSERT(!mPollerBackend);
  SOCKET_LOG(("nsSocketTransportService::MoveToIdleList %p [handler=%p]\n",
              sock, sock->mHandler.get()));
  MOZ_ASSERT(SockIndex(mIdleList, sock) == -1);
  MOZ_ASSERT(SockIndex(mActiveList, sock) != -1);
  AddToIdleList(sock);
  RemoveFromPollList(sock);
}

// TODO: remove once network.sts.use_nspr_for_polling is retired.
// Legacy PR_Poll path only -- never reached when mPollerBackend is set.
void nsSocketTransportService::MoveToPollList(SocketContext* sock) {
  MOZ_ASSERT(!mPollerBackend);
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

// Shared by both the legacy PR_Poll path and DoPollIterationWithBackend():
// under mPollerBackend, mActiveList holds every attached socket (idle and
// active alike), but DoPollIterationWithBackend() explicitly
// DisengageTimeout()s every idle one each iteration, so an idle socket's
// TimeoutIn() always returns the NS_SOCKET_POLL_TIMEOUT sentinel here and
// contributes a no-op to the minimum computed below.
PRIntervalTime nsSocketTransportService::PollTimeout(PRIntervalTime now) {
  if (mActiveList.IsEmpty()) {
    return NS_SOCKET_POLL_TIMEOUT;
  }

  // compute minimum time before any socket timeout expires.
  PRIntervalTime minR = NS_SOCKET_POLL_TIMEOUT;
  for (uint32_t i = 0; i < mActiveList.Length(); ++i) {
    const SocketContext& s = mActiveList[i];
    PRIntervalTime r = s.TimeoutIn(now);
    if (r < minR) {
      minR = r;
    }
  }
  if (minR == NS_SOCKET_POLL_TIMEOUT) {
    SOCKET_LOG(("poll timeout: none\n"));
    return NS_SOCKET_POLL_TIMEOUT;
  }
  SOCKET_LOG(("poll timeout: %" PRIu32 "\n", PR_IntervalToSeconds(minR)));
  return minR;
}

// TODO: remove once network.sts.use_nspr_for_polling is retired.
int32_t nsSocketTransportService::Poll(PRIntervalTime ts) {
  MOZ_ASSERT(IsOnCurrentThread());
  MOZ_ASSERT(!mPollerBackend, "Poll() is the PR_Poll-only path");
  PRPollDesc* firstPollEntry;
  uint32_t pollCount;
  PRIntervalTime pollTimeout;

  // If there are pending events for this thread then
  // DoPollIteration() should service the network without blocking.
  bool pendingEvents = false;
  mRawThread->HasPendingEvents(&pendingEvents);
  {
    AutoReadLock lock(mQueueLock);
    pendingEvents = pendingEvents || !mPriorityEventQueue.IsEmpty();
  }

  if (mPollList[0].fd) {
    mPollList[0].out_flags = 0;
    firstPollEntry = &mPollList[0];
    pollCount = mPollList.Length();
    pollTimeout = pendingEvents ? PR_INTERVAL_NO_WAIT : PollTimeout(ts);
  } else {
    // no pollable event, so busy wait...
    pollCount = mActiveList.Length();
    if (pollCount) {
      firstPollEntry = &mPollList[1];
    } else {
      firstPollEntry = nullptr;
    }
    pollTimeout =
        pendingEvents ? PR_INTERVAL_NO_WAIT : PR_MillisecondsToInterval(25);
  }

  if ((ts - mLastNetworkLinkChangeTime) < mNetworkLinkChangeBusyWaitPeriod) {
    // Being here means we are few seconds after a network change has
    // been detected.
    PRIntervalTime to = mNetworkLinkChangeBusyWaitTimeout;
    if (to) {
      pollTimeout = std::min(to, pollTimeout);
      SOCKET_LOG(("  timeout shorthened after network change event"));
    }
  }

  TimeStamp pollStart;
  if (Telemetry::CanRecordPrereleaseData()) {
    pollStart = TimeStamp::NowLoRes();
  }

  SOCKET_LOG(("    timeout = %i milliseconds\n",
              PR_IntervalToMilliseconds(pollTimeout)));

  int32_t n;
  {
    TimeStamp startTime = TimeStamp::Now();
    if (pollTimeout != PR_INTERVAL_NO_WAIT) {
      // There will be an actual non-zero wait, let the profiler know about it
      // by marking thread as sleeping around the polling call.
      profiler_thread_sleep();
    }

    PRIntervalTime t0 = PR_IntervalNow();
    n = PR_Poll(firstPollEntry, pollCount, pollTimeout);
    // Without the dev-only NSPR instrumentation patch, layer-walk time and
    // kernel-wait time cannot be separated for the nspr backend. The total
    // elapsed time is recorded in kernelWaitUs; layerWalkUs stays at 0.
    ++mPollerStats.iterations;
    mPollerStats.fdCount.Add(pollCount);
    mPollerStats.kernelWaitUs.Add(
        PR_IntervalToMicroseconds(PR_IntervalNow() - t0));
    mPollerStats.readyCount.Add(n > 0 ? static_cast<uint32_t>(n) : 0);

    if (pollTimeout != PR_INTERVAL_NO_WAIT) {
      profiler_thread_wake();
    }
    if (profiler_thread_is_being_profiled_for_markers()) {
      PROFILER_MARKER_TEXT(
          "SocketTransportService::Poll", NETWORK,
          MarkerTiming::IntervalUntilNowFrom(startTime),
          pollTimeout == PR_INTERVAL_NO_TIMEOUT
              ? nsPrintfCString("Poll count: %u, Poll timeout: NO_TIMEOUT",
                                pollCount)
          : pollTimeout == PR_INTERVAL_NO_WAIT
              ? nsPrintfCString("Poll count: %u, Poll timeout: NO_WAIT",
                                pollCount)
              : nsPrintfCString("Poll count: %u, Poll timeout: %ums", pollCount,
                                PR_IntervalToMilliseconds(pollTimeout)));
    }
  }

  SOCKET_LOG(("    ...returned after %i milliseconds\n",
              PR_IntervalToMilliseconds(PR_IntervalNow() - ts)));

  return n;
}

//-----------------------------------------------------------------------------
// xpcom api

NS_IMPL_ISUPPORTS(nsSocketTransportService, nsISocketTransportService,
                  nsIRoutedSocketTransportService, nsIEventTarget,
                  nsISerialEventTarget, nsIThreadObserver, nsIRunnable,
                  nsPISocketTransportService, nsIObserver, nsINamed,
                  nsIDirectTaskDispatcher)

static const char* gCallbackUpdatePrefs[] = {
    SEND_BUFFER_PREF,
    KEEPALIVE_ENABLED_PREF,
    KEEPALIVE_IDLE_TIME_PREF,
    KEEPALIVE_RETRY_INTERVAL_PREF,
    KEEPALIVE_PROBE_COUNT_PREF,
    MAX_TIME_BETWEEN_TWO_POLLS,
    MAX_TIME_FOR_PR_CLOSE_DURING_SHUTDOWN,
    POLLABLE_EVENT_TIMEOUT,
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

  // Create the backend before the socket thread starts so the thread sees it
  // via the thread-creation memory barrier.
  //
  // Deliberately not StaticPrefs::network_sts_use_nspr_for_polling_AtStartup():
  // this Init() can run very early (before Preferences::InitializeUserPrefs()
  // has parsed the profile/test-harness pref overrides -- e.g. via
  // nsIIOService's offline-mode path), and calling any StaticPrefs `once`
  // accessor for the first time anywhere in the process latches every
  // `once`-mirrored pref's value immediately (see
  // StaticPrefs::MaybeInitOncePrefs()), not just this one. Reached this
  // early, that pulls the global once-latch earlier than it would otherwise
  // run, so any other once-mirrored pref a test's own prefs.js override
  // still needs to change trips
  // "Preference '<name>' got modified since StaticPrefs::..._AtStartup was
  // initialized" -- observed in practice for gfx.webgpu.ignore-blocklist
  // across jsreftest/reftest/crashtest/mochitest-webgpu. mInitialized above
  // already guarantees this whole function body -- and so this read --
  // executes at most once per process, so a plain Preferences::GetBool()
  // gives the same "decide once" semantics without the global side effect.
  if (!Preferences::GetBool("network.sts.use_nspr_for_polling", false)) {
    mPollerBackend = MakeUnique<PollPollerBackend>();
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

  Preferences::RegisterCallbacks(UpdatePrefs, gCallbackUpdatePrefs, this);
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

  {
    MutexAutoLock lock(mLock);

    if (mPollableEvent) {
      mPollableEvent->Signal();
    }
  }

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

  // The socket thread has now fully stopped. Print accumulated polling stats.
  mPollerStats.Print(mPollerBackend ? "new" : "nspr");

  {
    MutexAutoLock lock(mLock);
    // Drop our reference to mThread and make sure that any concurrent readers
    // are excluded
    mThread = nullptr;
    mDirectTaskDispatcher = nullptr;
  }

  Preferences::UnregisterCallbacks(UpdatePrefs, gCallbackUpdatePrefs, this);

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
  if (mPollableEvent) {
    mPollableEvent->Signal();
  }

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
  // This check is redundant to one done inside ::Signal(), but we can do it
  // here and skip obtaining the lock - given that this is a relatively common
  // occurrence its worth the redundant code.
  if (OnSocketThread()) {
    SOCKET_LOG(("OnDispatchedEvent Same Thread Skip Signal\n"));
    return NS_OK;
  }
#ifdef XP_WIN
  if (gIOService->IsNetTearingDown()) {
    StartPollWatchdog();
  }
#endif

  MutexAutoLock lock(mLock);
  if (mPollableEvent) {
    mPollableEvent->Signal();
  }
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

  {
    // See bug 1843384:
    // Avoid blocking the main thread by allocating the PollableEvent outside
    // the mutex. Still has the potential to hang the socket thread, but the
    // main thread remains responsive.
    PollableEvent* pollable = new PollableEvent();
    MutexAutoLock lock(mLock);
    mPollableEvent.reset(pollable);

    //
    // NOTE: per bug 190000, this failure could be caused by Zone-Alarm
    // or similar software.
    //
    // NOTE: per bug 191739, this failure could also be caused by lack
    // of a loopback device on Windows and OS/2 platforms (it creates
    // a loopback socket pair on these platforms to implement a pollable
    // event object).  if we can't create a pollable event, then we'll
    // have to "busy wait" to implement the socket event queue :-(
    //
    if (!mPollableEvent->Valid()) {
      mPollableEvent = nullptr;
      NS_WARNING("running socket transport thread without a pollable event");
      SOCKET_LOG(("running socket transport thread without a pollable event"));
    }

    if (mPollerBackend) {
      if (mPollableEvent) {
        mPollableEventNativeFd =
            PR_FileDesc2NativeHandle(mPollableEvent->PollableFD());
        mPollerBackend->Add(mPollableEventNativeFd, kPollerRead,
                            reinterpret_cast<void*>(mPollableEventNativeFd));
      }
    } else {
      PRPollDesc entry = {
          mPollableEvent ? mPollableEvent->PollableFD() : nullptr,
          PR_POLL_READ | PR_POLL_EXCEPT, 0};
      SOCKET_LOG(("Setting entry 0"));
      mPollList[0] = entry;
    }
  }

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

      if (mPollerBackend) {
        DoPollIterationWithBackend();
      } else {
        DoPollIteration();
      }

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
  MOZ_ASSERT(mPollList.Length() == 1);
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

// TODO: remove once network.sts.use_nspr_for_polling is retired.
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
      uint16_t in_flags = mActiveList[i].mHandler->mPollFlags;
      if (in_flags == 0) {
        MoveToIdleList(&mActiveList[i]);
      } else {
        // update poll flags
        mPollList[i + 1].in_flags = in_flags;
        mPollList[i + 1].out_flags = 0;
        mActiveList[i].EnsureTimeout(now);
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
      MoveToPollList(&mIdleList[i]);
    }
  }

  {
    MutexAutoLock lock(mLock);
    if (mPollableEvent) {
      // we want to make sure the timeout is measured from the time
      // we enter poll().  This method resets the timestamp to 'now',
      // if we were first signalled between leaving poll() and here.
      // If we didn't do this and processing events took longer than
      // the allowed signal timeout, we would detect it as a
      // false-positive.  AdjustFirstSignalTimestamp is then a no-op
      // until mPollableEvent->Clear() is called.
      mPollableEvent->AdjustFirstSignalTimestamp();
    }
  }

  SOCKET_LOG(("  calling PR_Poll [active=%zu idle=%zu]\n", mActiveList.Length(),
              mIdleList.Length()));

  // Measures seconds spent while blocked on PR_Poll
  int32_t n = 0;

  if (!gIOService->IsNetTearingDown()) {
    // Let's not do polling during shutdown.
#if defined(XP_WIN)
    StartPolling();
#endif
    n = Poll(now);
#if defined(XP_WIN)
    EndPolling();
#endif
  }

  now = PR_IntervalNow();

  TimeStamp startTime;
  bool profiling = profiler_thread_is_being_profiled_for_markers();
  if (profiling) {
    startTime = TimeStamp::Now();
  }

  if (n < 0) {
    SOCKET_LOG(("  PR_Poll error [%d] os error [%d]\n", PR_GetError(),
                PR_GetOSError()));
  } else {
    //
    // service "active" sockets...
    //
    for (i = 0; i < int32_t(mActiveList.Length()); ++i) {
      PRPollDesc& desc = mPollList[i + 1];
      SocketContext& s = mActiveList[i];
      if (n > 0 && desc.out_flags != 0) {
        s.DisengageTimeout();
        s.mHandler->OnSocketReady(desc.fd, desc.out_flags);
      } else if (s.IsTimedOut(now)) {
        SOCKET_LOG(("socket %p timed out", s.mHandler.get()));
        s.DisengageTimeout();
        s.mHandler->OnSocketReady(desc.fd, -1);
      } else {
        s.MaybeResetEpoch();
      }
    }
    //
    // check for "dead" sockets and remove them (need to do this in
    // reverse order obviously).
    //
    for (i = mActiveList.Length() - 1; i >= 0; --i) {
      if (NS_FAILED(mActiveList[i].mHandler->mCondition)) {
        DetachSocket(mActiveList, &mActiveList[i]);
      }
    }

    {
      MutexAutoLock lock(mLock);
      // acknowledge pollable event (should not block)
      if (n != 0 &&
          (mPollList[0].out_flags & (PR_POLL_READ | PR_POLL_EXCEPT)) &&
          mPollableEvent &&
          ((mPollList[0].out_flags & PR_POLL_EXCEPT) ||
           !mPollableEvent->Clear())) {
        // On Windows, the TCP loopback connection in the
        // pollable event may become broken when a laptop
        // switches between wired and wireless networks or
        // wakes up from hibernation.  We try to create a
        // new pollable event.  If that fails, we fall back
        // on "busy wait".
        TryRepairPollableEvent();
      }

      if (mPollableEvent &&
          !mPollableEvent->IsSignallingAlive(mPollableEventTimeout)) {
        SOCKET_LOG(("Pollable event signalling failed/timed out"));
        TryRepairPollableEvent();
      }
    }
  }

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

  return NS_OK;
}

int32_t nsSocketTransportService::FindActiveIndexByNativeFD(
    PollerFd aFd) const {
  // -1 is PR_FileDesc2NativeHandle()'s sentinel for a PRFileDesc with no
  // real OS-backed native handle (e.g. a synthetic PR_CreateIOLayerStub fd
  // that didn't opt out via NO_NATIVE_HANDLE). Never match on it: two
  // distinct synthetic fds would otherwise collide with each other, or with
  // whichever real socket a caller happens to check first.
  if (aFd < 0) {
    return -1;
  }
  for (uint32_t i = 0; i < mActiveList.Length(); ++i) {
    if (PR_FileDesc2NativeHandle(mActiveList[i].mFD) == aFd) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

// Same overall shape as DoPollIteration(), but delegates the kernel wait to
// mPollerBackend instead of PR_Poll, and resolves each socket's NSPR-layer
// interest itself (WalkSocketLayers()) rather than relying on PR_Poll to do
// it internally. See the design notes in Poller.h for why this per-iteration
// walk exists and what removes it in a later increment.
//
// Unlike DoPollIteration(), there is no separate idle list to rebalance:
// mActiveList holds every attached socket regardless of current interest in
// this mode, and WalkSocketLayers() is called for all of them every
// iteration (it short-circuits immediately for a socket with no requested
// interest, so this costs nothing extra for a quiescent one) -- see the
// comment above SocketContext for why this backend doesn't need the
// legacy idle/active split. PollTimeout() is still reused as-is despite
// mActiveList now including quiescent sockets: TimeoutIn() returns the
// NS_SOCKET_POLL_TIMEOUT sentinel for any socket whose timeout was never
// engaged (mPollStartEpoch == 0). Unlike the legacy path -- where a socket
// going idle is physically moved out of the timeout-checked set -- nothing
// here removes a quiescent socket from mActiveList, so the detach/engage
// pass below must explicitly DisengageTimeout() it; otherwise a socket that
// was active (and had EnsureTimeout()'d a real epoch) before going idle
// would carry that stale, still-ticking epoch and could be spuriously
// reported as timed out once enough wall-clock time passes while idle.
nsresult nsSocketTransportService::DoPollIterationWithBackend() {
  SOCKET_LOG(("STS poll iter (backend)\n"));

  PRIntervalTime now = PR_IntervalNow();
  int32_t i;

  // Detach dead sockets; engage the timeout of ones with real interest and
  // disengage it for ones without (see the function comment above for why
  // the latter is necessary here but not in DoPollIteration()). Backward
  // iteration for UnorderedRemoveElementAt safety, matching
  // DoPollIteration()'s equivalent pass.
  for (i = mActiveList.Length() - 1; i >= 0; --i) {
    if (NS_FAILED(mActiveList[i].mHandler->mCondition)) {
      DetachSocket(mActiveList, &mActiveList[i]);
    } else if (mActiveList[i].mHandler->mPollFlags != 0) {
      mActiveList[i].EnsureTimeout(now);
    } else {
      mActiveList[i].DisengageTimeout();
    }
  }

  {
    MutexAutoLock lock(mLock);
    if (mPollableEvent) {
      mPollableEvent->AdjustFirstSignalTimestamp();
    }
  }

  SOCKET_LOG(("  backend poll [active=%zu]\n", mActiveList.Length()));

  uint32_t activeCount = mActiveList.Length();
  mDirectionMaps.SetLength(activeCount);
  AutoTArray<int16_t, 64> outFlags;
  outFlags.SetLength(activeCount);
  for (auto& f : outFlags) f = 0;

  TimeStamp layerWalkStart = TimeStamp::Now();
  int32_t shortCircuitCount = 0;
  for (i = 0; i < int32_t(activeCount); ++i) {
    SocketContext& s = mActiveList[i];
    if (s.mNSSReadinessManaged) {
      // NSS pushes this socket's interest via OnTLSReadinessChanged() from
      // its own I/O choke points, independent of poll-loop cadence; walking
      // it here too would race that push with a redundant pull. Its
      // mDirectionMaps/outFlags entry is filled from the socket's own
      // mReadiness in the Wait()-result loop below instead.
      mDirectionMaps[i] = 0;
      if (PollerInterestFromReadiness(s.mReadiness) == 0) {
        // Zero OS interest here means this fd's NSPR layer stack (e.g.
        // nsSSLIOLayerPoll(), above NSS's own SSL layer) won't otherwise get
        // polled at all while paused. Some pre-existing PSM layers rely on
        // being incidentally re-polled every iteration for side effects
        // unrelated to their returned interest (e.g. kicking off client
        // certificate selection once it's pending, or nsSSLIOLayerPoll's own
        // "certificate validation already failed" branch, which returns
        // PR_POLL_EXCEPT directly to prompt an immediate read/write without
        // ever consulting NSS's ssl_Poll) -- calling poll() here preserves
        // that legacy behavior without reintroducing WalkSocketLayers()/
        // Modify() for this fd. A non-zero result is exactly that kind of
        // short-circuit signal, so feed it into outFlags like
        // WalkSocketLayers()'s own shortCircuited path does; unlike that
        // path there is no scratch/direction-map to go with it, since this
        // isn't an OS-level event mapped back through UnmapReadyFlags(). A
        // zero result costs nothing beyond this call chain: ssl_Poll() itself
        // won't cascade to a real OS-level poll() while restartTarget is
        // set, so this never reaches a syscall.
        // TODO: revisit if this shows up in profiles -- could be made
        // one-shot (e.g. only after this fd's next OnSocketReady()) instead
        // of every iteration while paused.
        //
        // Skip entirely when mPollFlags == 0 (idle -- see the "make idle"
        // assignments in nsSocketTransport2.cpp), matching WalkSocketLayers()'s
        // own "!aWantFlags" early return just above and the prior Rust PoC's
        // equivalent idle-list guard: nsSSLIOLayerPoll()'s cert-validation-
        // failed/shutdown branch asserts in_flags has PR_POLL_EXCEPT, which a
        // genuinely idle socket's mPollFlags (0) never has, and there is no
        // direction for OnSocketReady() to deliver a result to anyway when
        // idle -- its own mPollFlags-gated checks below reject it regardless
        // of what compat-poll would have returned.
        if (s.mHandler->mPollFlags != 0) {
          int16_t compatOutFlags = 0;
          s.mFD->methods->poll(s.mFD, s.mHandler->mPollFlags, &compatOutFlags);
          if (compatOutFlags != 0) {
            if (outFlags[i] == 0) {
              ++shortCircuitCount;
            }
            outFlags[i] |= compatOutFlags;
          }
        }
      }
      continue;
    }
    LayerWalkResult r = WalkSocketLayers(s.mFD, s.mHandler->mPollFlags);
    if (r.shortCircuited) {
      outFlags[i] = r.outFlags;
      mDirectionMaps[i] = 0;
      ++shortCircuitCount;
    } else {
      mDirectionMaps[i] = r.directionMap;
      PollerFd fd = PR_FileDesc2NativeHandle(s.mFD);
      DebugOnly<nsresult> rv = mPollerBackend->Modify(fd, r.osInterest);
      MOZ_ASSERT(NS_SUCCEEDED(rv),
                "active socket not registered with mPollerBackend");
    }
  }
  mPollerStats.layerWalkUs.Add(
      static_cast<uint64_t>((TimeStamp::Now() - layerWalkStart).ToMicroseconds()));
  mPollerStats.fdCount.Add(activeCount);

  // Ready-now list (see OnTLSReadinessChanged()): decrypted plaintext is
  // already buffered, or the connection has permanently failed, for these
  // fds, so service them this iteration exactly like a WalkSocketLayers()
  // short-circuit -- no OS-level event needed or waited for. Backward
  // iteration for the RemoveElementAt() below.
  for (int32_t j = mReadyNowList.Length() - 1; j >= 0; --j) {
    PollerFd fd = mReadyNowList[j];
    int32_t idx = FindActiveIndexByNativeFD(fd);
    if (idx < 0) {
      continue;  // stale; DetachSocket() already removes live entries
    }
    const SSLReadiness& readiness = mActiveList[idx].mReadiness;
    // Only report a direction the handler actually asked for. Forcing
    // PR_POLL_READ regardless of mPollFlags left entries stuck here whenever
    // the handler wasn't currently interested in reading: plaintextReady
    // clears only via a fresh NSS callback, which requires real I/O activity
    // that a handler not currently reading won't generate, yet outFlags was
    // still forced non-zero here, tripping shortCircuitCount below every
    // iteration -- an indefinite busy-spin skipping the kernel Wait() for a
    // notification nsSocketTransport::OnSocketReady()'s own mPollFlags gate
    // would just drop. terminallyFailed uses PR_POLL_EXCEPT, matching the
    // legacy PSM failed-cert-validation short-circuit noted above, since
    // that gate accepts it against either direction the handler is waiting
    // on, not specifically read.
    int16_t event = 0;
    if (readiness.plaintextReady && (mActiveList[idx].mHandler->mPollFlags &
                                      PR_POLL_READ)) {
      event |= PR_POLL_READ;
    }
    if (readiness.terminallyFailed) {
      event |= PR_POLL_EXCEPT;
    }
    if (event != 0) {
      if (outFlags[idx] == 0) {
        ++shortCircuitCount;
      }
      outFlags[idx] |= event;
    }
    // Unlike plaintextReady, terminallyFailed never clears via a fresh
    // readiness event -- nothing will ever call the readiness callback
    // again for a connection that's permanently done -- so it must be
    // serviced at most once here, or this fd would short-circuit the
    // kernel Wait() and spin the poll loop on it forever. This has to fire
    // on terminallyFailed alone: gating it on !plaintextReady too meant a
    // connection that failed with leftover buffered plaintext was never
    // removed, since terminallyFailed's defining property is that no future
    // callback will ever recompute plaintextReady to false either. The one
    // shot above already folds in PR_POLL_READ for that buffered data (if
    // the handler wants it), so removing here doesn't lose it.
    if (readiness.terminallyFailed) {
      mReadyNowList.RemoveElementAt(j);
      mActiveList[idx].mOnReadyNowList = false;
    }
  }

  bool pollableEventReady = false;
  int16_t pollableEventOutFlagsPR = 0;
  int32_t n = 0;

  if (shortCircuitCount > 0) {
    // Mirrors the layer-short-circuit path in Poller::Wait: at least one
    // socket is already ready with no OS-level event, so skip the kernel
    // call entirely this iteration; the others are re-walked next time.
    ++mPollerStats.iterations;
    ++mPollerStats.shortCircuits;
    mPollerStats.kernelWaitUs.Add(0);
    mPollerStats.readyCount.Add(static_cast<uint32_t>(shortCircuitCount));
    n = shortCircuitCount;
  } else if (!gIOService->IsNetTearingDown()) {
    bool pendingEvents = false;
    mRawThread->HasPendingEvents(&pendingEvents);
    {
      AutoReadLock qlock(mQueueLock);
      pendingEvents = pendingEvents || !mPriorityEventQueue.IsEmpty();
    }

    TimeDuration timeout;
    if (pendingEvents) {
      timeout = TimeDuration::FromSeconds(0);
    } else {
      PRIntervalTime minTimeout = PollTimeout(now);
      timeout = minTimeout == NS_SOCKET_POLL_TIMEOUT
                    ? TimeDuration::Forever()
                    : TimeDuration::FromMilliseconds(
                          PR_IntervalToMilliseconds(minTimeout));
    }

    if ((now - mLastNetworkLinkChangeTime) <
        mNetworkLinkChangeBusyWaitPeriod) {
      PRIntervalTime to = mNetworkLinkChangeBusyWaitTimeout;
      if (to) {
        TimeDuration toDuration =
            TimeDuration::FromMilliseconds(PR_IntervalToMilliseconds(to));
        if (toDuration < timeout) {
          timeout = toDuration;
        }
      }
    }

#if defined(XP_WIN)
    StartPolling();
#endif
    nsTArray<PollerReadyEvent> ready;
    n = mPollerBackend->Wait(timeout, ready, &mPollerStats);
#if defined(XP_WIN)
    EndPolling();
#endif

    if (n > 0) {
      for (auto& ev : ready) {
        PollerFd fd = static_cast<PollerFd>(reinterpret_cast<intptr_t>(ev.key));
        if (fd == mPollableEventNativeFd) {
          pollableEventReady = true;
          if (ev.outFlags & kPollerRead) pollableEventOutFlagsPR |= PR_POLL_READ;
          if (ev.outFlags & kPollerExcept) {
            pollableEventOutFlagsPR |= PR_POLL_EXCEPT;
          }
          continue;
        }
        int32_t idx = FindActiveIndexByNativeFD(fd);
        if (idx < 0) {
          continue;  // detached mid-flight; ignore
        }
        const SocketContext& s = mActiveList[idx];
        int16_t directionMap = mDirectionMaps[idx];
        if (s.mNSSReadinessManaged) {
          // No WalkSocketLayers() direction map for this fd (skipped
          // above); pack the equivalent mapping from NSS's own
          // last-reported readiness.
          directionMap = DirectionMapFromReadiness(s.mReadiness);
        }
        outFlags[idx] = UnmapReadyFlags(ev.outFlags, directionMap);
      }
    }
  }

  now = PR_IntervalNow();

  TimeStamp startTime;
  bool profiling = profiler_thread_is_being_profiled_for_markers();
  if (profiling) {
    startTime = TimeStamp::Now();
  }

  if (n < 0) {
    SOCKET_LOG(("  backend poll error\n"));
  } else {
    // Bound by whichever of mActiveList's live length (re-evaluated every
    // iteration, matching DoPollIteration(), in case a handler's
    // OnSocketReady() reentrantly detaches something and shrinks the list)
    // or activeCount (outFlags/mDirectionMaps's fixed size) is smaller.
    // A socket reentrantly attached mid-loop is simply serviced on the next
    // iteration instead of this one.
    for (i = 0; i < int32_t(std::min<uint32_t>(mActiveList.Length(),
                                               activeCount));
         ++i) {
      SocketContext& s = mActiveList[i];
      if (n != 0 && outFlags[i] != 0) {
        s.DisengageTimeout();
        s.mHandler->OnSocketReady(s.mFD, outFlags[i]);
      } else if (s.IsTimedOut(now)) {
        SOCKET_LOG(("socket %p timed out", s.mHandler.get()));
        s.DisengageTimeout();
        s.mHandler->OnSocketReady(s.mFD, -1);
      } else {
        s.MaybeResetEpoch();
      }
    }
    for (i = mActiveList.Length() - 1; i >= 0; --i) {
      if (NS_FAILED(mActiveList[i].mHandler->mCondition)) {
        DetachSocket(mActiveList, &mActiveList[i]);
      }
    }

    {
      MutexAutoLock lock(mLock);
      if (n != 0 && pollableEventReady &&
          (pollableEventOutFlagsPR & (PR_POLL_READ | PR_POLL_EXCEPT)) &&
          mPollableEvent &&
          ((pollableEventOutFlagsPR & PR_POLL_EXCEPT) ||
           !mPollableEvent->Clear())) {
        TryRepairPollableEvent();
      }
      if (mPollableEvent &&
          !mPollableEvent->IsSignallingAlive(mPollableEventTimeout)) {
        SOCKET_LOG(("Pollable event signalling failed/timed out"));
        TryRepairPollableEvent();
      }
    }
  }

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

  int32_t pollableEventTimeout;
  rv = Preferences::GetInt(POLLABLE_EVENT_TIMEOUT, &pollableEventTimeout);
  if (NS_SUCCEEDED(rv) && pollableEventTimeout >= 0) {
    MutexAutoLock lock(mLock);
    mPollableEventTimeout = TimeDuration::FromSeconds(pollableEventTimeout);
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

#if defined(XP_WIN)
    if (timer == mPollRepairTimer) {
      DoPollRepair();
    }
#endif

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
    if (mActiveList[i].mHandler->mOriginAttributes.IsPrivateBrowsing()) {
      DetachSocket(mActiveList, &mActiveList[i]);
    }
  }
  for (int32_t i = mIdleList.Length() - 1; i >= 0; --i) {
    if (mIdleList[i].mHandler->mOriginAttributes.IsPrivateBrowsing()) {
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
    gMaxCount = rlimitData.rlim_cur;
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
  nsCString originAttributesSuffix;
  context->mHandler->mOriginAttributes.CreateSuffix(originAttributesSuffix);
  SocketInfo info = {nsCString(host),
                     sent,
                     received,
                     port,
                     aActive,
                     nsCString(type_desc),
                     originAttributesSuffix};

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

#if defined(XP_WIN)
void nsSocketTransportService::StartPollWatchdog() {
  // Start off the timer from a runnable off of the main thread in order to
  // avoid a deadlock, see bug 1370448.
  RefPtr<nsSocketTransportService> self(this);
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "nsSocketTransportService::StartPollWatchdog", [self] {
        MutexAutoLock lock(self->mLock);

        // Poll can hang sometimes. If we are in shutdown, we are going to start
        // a watchdog. If we do not exit poll within REPAIR_POLLABLE_EVENT_TIME
        // signal a pollable event again.
        if (gIOService->IsNetTearingDown() && self->mPolling &&
            !self->mPollRepairTimer) {
          NS_NewTimerWithObserver(getter_AddRefs(self->mPollRepairTimer), self,
                                  REPAIR_POLLABLE_EVENT_TIME,
                                  nsITimer::TYPE_REPEATING_SLACK);
        }
      }));
}

void nsSocketTransportService::DoPollRepair() {
  MutexAutoLock lock(mLock);
  if (mPolling && mPollableEvent) {
    mPollableEvent->Signal(/* aForce = */ true);
  } else if (mPollRepairTimer) {
    mPollRepairTimer->Cancel();
  }
}

void nsSocketTransportService::StartPolling() {
  MutexAutoLock lock(mLock);
  mPolling = true;
}

void nsSocketTransportService::EndPolling() {
  MutexAutoLock lock(mLock);
  mPolling = false;
  if (mPollRepairTimer) {
    mPollRepairTimer->Cancel();
  }
}

#endif

void nsSocketTransportService::TryRepairPollableEvent() MOZ_REQUIRES(mLock) {
  mLock.AssertCurrentThreadOwns();

  PollerFd oldNativeFd = mPollableEventNativeFd;

  PollableEvent* pollable = nullptr;
  {
    // Bug 1719046: In certain cases PollableEvent constructor can hang
    // when callign PR_NewTCPSocketPair.
    // We unlock the mutex to prevent main thread hangs acquiring the lock.
    MutexAutoUnlock unlock(mLock);
    pollable = new PollableEvent();
  }

  NS_WARNING("Trying to repair mPollableEvent");
  mPollableEvent.reset(pollable);
  if (!mPollableEvent->Valid()) {
    mPollableEvent = nullptr;
  }
  SOCKET_LOG(
      ("running socket transport thread without "
       "a pollable event now valid=%d",
       !!mPollableEvent));

  if (mPollerBackend) {
    if (oldNativeFd != -1) {
      mPollerBackend->Remove(oldNativeFd);
    }
    mPollableEventNativeFd = -1;
    if (mPollableEvent) {
      mPollableEventNativeFd =
          PR_FileDesc2NativeHandle(mPollableEvent->PollableFD());
      mPollerBackend->Add(mPollableEventNativeFd, kPollerRead,
                          reinterpret_cast<void*>(mPollableEventNativeFd));
    }
  } else {
    mPollList[0].fd = mPollableEvent ? mPollableEvent->PollableFD() : nullptr;
    mPollList[0].in_flags = PR_POLL_READ | PR_POLL_EXCEPT;
    mPollList[0].out_flags = 0;
  }
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

}  // namespace net
}  // namespace mozilla
