/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef NeckoPollGeckoArgs_h
#define NeckoPollGeckoArgs_h

// Helpers for passing the cross-thread notify socket pair to child processes
// via geckoargs.  The parent process calls NeckoPollPutHandlesToArgs() before
// launching each sandboxed child; each child calls NeckoPollConsumeHandles()
// before calling SandboxTarget::StartSandbox().

#if defined(XP_WIN) && defined(MOZ_SANDBOX)

#  include "mozilla/GeckoArgs.h"
#  include "mozilla/net/necko_poll.h"
#  include "nsDebug.h"

namespace mozilla::net {

[[nodiscard]] inline bool NeckoPollPutHandlesToArgs(
    geckoargs::ChildProcessArgs& aArgs) {
  uintptr_t notifyRead = 0, notifyWrite = 0;
  if (!necko_poll_create_notify_pair_for_child(&notifyRead, &notifyWrite)) {
    NS_WARNING("necko_poll_create_notify_pair_for_child failed");
    return false;
  }
  geckoargs::sNeckoNotifyRead.Put(
      UniqueFileHandle(reinterpret_cast<HANDLE>(notifyRead)), aArgs);
  geckoargs::sNeckoNotifyWrite.Put(
      UniqueFileHandle(reinterpret_cast<HANDLE>(notifyWrite)), aArgs);
  return true;
}

inline void NeckoPollConsumeHandles(int aArgc, char* aArgv[]) {
  auto r = geckoargs::sNeckoNotifyRead.Get(aArgc, aArgv);
  auto w = geckoargs::sNeckoNotifyWrite.Get(aArgc, aArgv);
  if (r.isSome() && w.isSome()) {
    auto rv =
        static_cast<uintptr_t>(static_cast<std::intptr_t>(r->release()));
    auto wv =
        static_cast<uintptr_t>(static_cast<std::intptr_t>(w->release()));
    necko_poll_set_pre_notify_pair(rv, wv);
  }
}

}  // namespace mozilla::net

#endif  // defined(XP_WIN) && defined(MOZ_SANDBOX)

#endif  // NeckoPollGeckoArgs_h
