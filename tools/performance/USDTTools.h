/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_USDTTools_h
#define mozilla_USDTTools_h

#include "mozilla/BaseProfilerMarkers.h"

#ifdef MOZ_USDT

#  include "mozilla/TimeStamp.h"

void InitUSDT();
void EmitUSDTMarkerProbeRaw(const char* aName, uint32_t aCategoryId,
                             const char* aTypeName, uint8_t aPhase,
                             uint64_t aStartNs, uint64_t aEndNs);

template <typename MarkerType, typename = void>
struct MarkerHasName : std::false_type {};
template <typename T>
struct MarkerHasName<T, std::void_t<decltype(T::Name)>> : std::true_type {};

template <typename MarkerType, typename... PayloadArguments>
void EmitUSDTMarkerProbe(
    const mozilla::ProfilerString8View& aName,
    const mozilla::MarkerCategory& aCategory,
    const mozilla::MarkerOptions& aOptions, MarkerType aMarkerType,
    const PayloadArguments&... aPayloadArguments) {
  const char* nameStr = aName.StringView().data();
  if (!nameStr) {
    nameStr = "";
  }

  uint32_t categoryId = static_cast<uint32_t>(aCategory.GetCategory());

  const char* markerTypeName;
  if constexpr (MarkerHasName<MarkerType>::value) {
    markerTypeName = MarkerType::Name;
  } else {
    markerTypeName = "unknown";
  }

  uint64_t startNs = 0, endNs = 0;
  uint8_t phase;

  if (aOptions.IsTimingUnspecified()) {
    startNs =
        mozilla::TimeStamp::Now().RawClockMonotonicNanosecondsSinceBoot();
    phase = 0;  // Instant
  } else {
    const auto& timing = aOptions.Timing();
    if (!timing.StartTime().IsNull()) {
      startNs = timing.StartTime().RawClockMonotonicNanosecondsSinceBoot();
    }
    if (!timing.EndTime().IsNull()) {
      endNs = timing.EndTime().RawClockMonotonicNanosecondsSinceBoot();
    }
    phase = timing.GetPhase();
  }

  EmitUSDTMarkerProbeRaw(nameStr, categoryId, markerTypeName, phase, startNs,
                          endNs);
}

#else  // !MOZ_USDT

inline void InitUSDT() {}

template <typename MarkerType, typename... PayloadArguments>
inline void EmitUSDTMarkerProbe(const mozilla::ProfilerString8View&,
                                const mozilla::MarkerCategory&,
                                const mozilla::MarkerOptions&, MarkerType,
                                const PayloadArguments&...) {}

#endif  // MOZ_USDT
#endif  // mozilla_USDTTools_h
