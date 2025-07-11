/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_DateTimeFormat_h
#define builtin_intl_DateTimeFormat_h

#include "builtin/SelfHostingDefines.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/TimeZone.h"
#include "js/Class.h"
#include "vm/NativeObject.h"

namespace mozilla::intl {
class DateTimeFormat;
class DateIntervalFormat;
}  // namespace mozilla::intl

namespace js {

enum class DateTimeValueKind {
  Number,
  TemporalDate,
  TemporalTime,
  TemporalDateTime,
  TemporalYearMonth,
  TemporalMonthDay,
  TemporalZonedDateTime,
  TemporalInstant,
};

class DateTimeFormatObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t INTERNALS_SLOT = 0;
  static constexpr uint32_t DATE_FORMAT_SLOT = 1;
  static constexpr uint32_t DATE_INTERVAL_FORMAT_SLOT = 2;
  static constexpr uint32_t DATE_TIME_VALUE_KIND_SLOT = 3;
  static constexpr uint32_t CALENDAR_SLOT = 4;
  static constexpr uint32_t TIMEZONE_SLOT = 5;
  static constexpr uint32_t SLOT_COUNT = 6;

  static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                "INTERNALS_SLOT must match self-hosting define for internals "
                "object slot");

  // Estimated memory use for UDateFormat (see IcuMemoryUsage).
  static constexpr size_t UDateFormatEstimatedMemoryUse = 72440;

  // Estimated memory use for UDateIntervalFormat (see IcuMemoryUsage).
  static constexpr size_t UDateIntervalFormatEstimatedMemoryUse = 175646;

  mozilla::intl::DateTimeFormat* getDateFormat() const {
    const auto& slot = getFixedSlot(DATE_FORMAT_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<mozilla::intl::DateTimeFormat*>(slot.toPrivate());
  }

  void setDateFormat(mozilla::intl::DateTimeFormat* dateFormat) {
    setFixedSlot(DATE_FORMAT_SLOT, PrivateValue(dateFormat));
  }

  mozilla::intl::DateIntervalFormat* getDateIntervalFormat() const {
    const auto& slot = getFixedSlot(DATE_INTERVAL_FORMAT_SLOT);
    if (slot.isUndefined()) {
      return nullptr;
    }
    return static_cast<mozilla::intl::DateIntervalFormat*>(slot.toPrivate());
  }

  void setDateIntervalFormat(
      mozilla::intl::DateIntervalFormat* dateIntervalFormat) {
    setFixedSlot(DATE_INTERVAL_FORMAT_SLOT, PrivateValue(dateIntervalFormat));
  }

  DateTimeValueKind getDateTimeValueKind() const {
    const auto& slot = getFixedSlot(DATE_TIME_VALUE_KIND_SLOT);
    if (slot.isUndefined()) {
      return DateTimeValueKind::Number;
    }
    return static_cast<DateTimeValueKind>(slot.toInt32());
  }

  void setDateTimeValueKind(DateTimeValueKind kind) {
    setFixedSlot(DATE_TIME_VALUE_KIND_SLOT,
                 Int32Value(static_cast<int32_t>(kind)));
  }

  temporal::CalendarValue getCalendar() const {
    const auto& slot = getFixedSlot(CALENDAR_SLOT);
    if (slot.isUndefined()) {
      return temporal::CalendarValue();
    }
    return temporal::CalendarValue(slot);
  }

  void setCalendar(const temporal::CalendarValue& calendar) {
    setFixedSlot(CALENDAR_SLOT, calendar.toSlotValue());
  }

  temporal::TimeZoneValue getTimeZone() const {
    const auto& slot = getFixedSlot(TIMEZONE_SLOT);
    if (slot.isUndefined()) {
      return temporal::TimeZoneValue();
    }
    return temporal::TimeZoneValue(slot);
  }

  void setTimeZone(const temporal::TimeZoneValue& timeZone) {
    setFixedSlot(TIMEZONE_SLOT, timeZone.toSlotValue());
  }

  void maybeClearCache(DateTimeValueKind kind);

 private:
  static const JSClassOps classOps_;
  static const ClassSpec classSpec_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

/**
 * Returns an array with the calendar type identifiers per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * supported calendars for the given locale. The default calendar is
 * element 0.
 *
 * Usage: calendars = intl_availableCalendars(locale)
 */
[[nodiscard]] extern bool intl_availableCalendars(JSContext* cx, unsigned argc,
                                                  JS::Value* vp);

/**
 * Returns the calendar type identifier per Unicode Technical Standard 35,
 * Unicode Locale Data Markup Language, for the default calendar for the given
 * locale.
 *
 * Usage: calendar = intl_defaultCalendar(locale)
 */
[[nodiscard]] extern bool intl_defaultCalendar(JSContext* cx, unsigned argc,
                                               JS::Value* vp);

/**
 * Returns a String value representing x (which must be a Number value)
 * according to the effective locale and the formatting options of the
 * given DateTimeFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 *
 * Usage: formatted = intl_FormatDateTime(dateTimeFormat, x, formatToParts)
 */
[[nodiscard]] extern bool intl_FormatDateTime(JSContext* cx, unsigned argc,
                                              JS::Value* vp);

/**
 * Returns a String value representing the range between x and y (which both
 * must be Number values) according to the effective locale and the formatting
 * options of the given DateTimeFormat.
 *
 * Spec: Intl.DateTimeFormat.prototype.formatRange proposal
 *
 * Usage: formatted = intl_FormatDateTimeRange(dateTimeFmt, x, y, formatToParts)
 */
[[nodiscard]] extern bool intl_FormatDateTimeRange(JSContext* cx, unsigned argc,
                                                   JS::Value* vp);

/**
 * Extracts the resolved components from a DateTimeFormat and applies them to
 * the object for resolved components.
 *
 * Usage: intl_resolveDateTimeFormatComponents(dateTimeFormat, resolved)
 */
[[nodiscard]] extern bool intl_resolveDateTimeFormatComponents(JSContext* cx,
                                                               unsigned argc,
                                                               JS::Value* vp);

namespace intl {

enum class DateTimeFormatKind {
  /**
   * Call CreateDateTimeFormat with `required = Any` and `defaults = All`.
   */
  All,

  /**
   * Call CreateDateTimeFormat with `required = Date` and `defaults = Date`.
   */
  Date,

  /**
   * Call CreateDateTimeFormat with `required = Time` and `defaults = Time`.
   */
  Time,
};

/**
 * Returns a new instance of the standard built-in DateTimeFormat constructor.
 */
[[nodiscard]] extern DateTimeFormatObject* CreateDateTimeFormat(
    JSContext* cx, JS::Handle<JS::Value> locales, JS::Handle<JS::Value> options,
    DateTimeFormatKind kind);

/**
 * Returns a possibly cached instance of the standard built-in DateTimeFormat
 * constructor.
 */
[[nodiscard]] extern DateTimeFormatObject* GetOrCreateDateTimeFormat(
    JSContext* cx, JS::Handle<JS::Value> locales, JS::Handle<JS::Value> options,
    DateTimeFormatKind kind);

/**
 * Returns a String value representing |millis| (which must be a valid time
 * value) according to the effective locale and the formatting options of the
 * given DateTimeFormat.
 */
[[nodiscard]] extern bool FormatDateTime(
    JSContext* cx, JS::Handle<DateTimeFormatObject*> dateTimeFormat,
    double millis, JS::MutableHandle<JS::Value> result);

/**
 * Shared `toLocaleString` implementation for Temporal objects.
 */
[[nodiscard]] extern bool TemporalObjectToLocaleString(
    JSContext* cx, const JS::CallArgs& args, DateTimeFormatKind formatKind,
    JS::Handle<JS::Value> toLocaleStringTimeZone = JS::UndefinedHandleValue);

}  // namespace intl

}  // namespace js

#endif /* builtin_intl_DateTimeFormat_h */
