/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/USDTTools.h"

#ifdef MOZ_USDT
#  include <stdlib.h>
#  include <sys/sdt.h>
#  include "mozilla/ProfilerState.h"

void InitUSDT() {
  if (getenv("MOZ_ENABLE_USDT")) {
    mozilla::profiler::detail::RacyFeatures::SetUSDTEnabled();
  }
}

void EmitUSDTMarkerProbeRaw(const char* aName, uint32_t aCategoryId,
                             const char* aTypeName, uint8_t aPhase,
                             uint64_t aStartNs, uint64_t aEndNs) {
  DTRACE_PROBE6(firefox, marker, aName, aCategoryId, aTypeName, aPhase,
                aStartNs, aEndNs);
}
#endif
