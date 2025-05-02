/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::{
    ffi,
    time::{SystemTime, UNIX_EPOCH},
};

use crate::prtypes::{PRBool, PRInt16, PRInt32, PRInt64, PRInt8, PRStatus, PRUint32};

/*
 *----------------------------------------------------------------------
 *
 * prtime.h --
 *
 *     NSPR date and time functions
 *
 *-----------------------------------------------------------------------
 */

/********************************************************************* */
/************************* TYPES AND CONSTANTS *********************** */
/********************************************************************* */

pub const PR_MSEC_PER_SEC: PRUint32 = 1000;
pub const PR_USEC_PER_SEC: PRUint32 = 1_000_000;
pub const PR_NSEC_PER_SEC: PRUint32 = 1_000_000_000;
pub const PR_USEC_PER_MSEC: PRUint32 = 1000;
pub const PR_NSEC_PER_MSEC: PRUint32 = 1_000_000;

/// NSPR represents basic time as 64-bit signed integers relative
/// to midnight (00:00:00), January 1, 1970 Greenwich Mean Time (GMT).
/// (GMT is also known as Coordinated Universal Time, UTC.)
/// The units of time are in microseconds. Negative times are allowed
/// to represent times prior to the January 1970 epoch. Such values are
/// intended to be exported to other systems or converted to human
/// readable form.
pub type PRTime = PRInt64;

/*
 * Time zone and daylight saving time corrections applied to GMT to
 * obtain the local time of some geographic location
 */
#[repr(C)]
pub struct PRTimeParameters {
    tp_gmt_offset: PRInt32, /* the offset from GMT in seconds */
    tp_dst_offset: PRInt32, /* contribution of DST in seconds */
}

/*
 * PRExplodedTime --
 *
 * Time broken down into human-readable components such as year, month,
 * day, hour, minute, second, and microsecond.  Time zone and daylight
 * saving time corrections may be applied.  If they are applied, the
 * offsets from the GMT must be saved in the 'tm_params' field so that
 * all the information is available to reconstruct GMT.
 *
 * Notes on porting: PRExplodedTime corrresponds to struct tm in
 * ANSI C, with the following differences:
 * - an additional field tm_usec;
 * - replacing tm_isdst by tm_params;
 * - the month field is spelled tm_month, not tm_mon;
 * - we use absolute year, AD, not the year since 1900.
 * The corresponding type in NSPR 1.0 is called PRTime.  Below is
 * a table of date/time type correspondence in the three APIs:
 * API          time since epoch          time in components
 * ANSI C             time_t                  struct tm
 * NSPR 1.0           PRInt64                   PRTime
 * NSPR 2.0           PRTime                  PRExplodedTime
 */
#[repr(C)]
#[expect(clippy::struct_field_names, reason = "NSPR code.")]
pub struct PRExplodedTime {
    tm_usec: PRInt32, /* microseconds past tm_sec (0-99999) */
    tm_sec: PRInt32,  /* seconds past tm_min (0-61, accomodating
                      up to two leap seconds) */
    tm_min: PRInt32,  /* minutes past tm_hour (0-59) */
    tm_hour: PRInt32, /* hours past tm_day (0-23) */
    tm_mday: PRInt32, /* days past tm_mon (1-31, note that it
                      starts from 1) */
    tm_month: PRInt32, /* months past tm_year (0-11, Jan = 0) */
    tm_year: PRInt16,  /* absolute year, AD (note that we do not
                       count from 1900) */

    tm_wday: PRInt8, /* calculated day of the week
                     (0-6, Sun = 0) */
    tm_yday: PRInt16, /* calculated day of the year
                      (0-365, Jan 1 = 0) */

    tm_params: PRTimeParameters, /* time parameters used by conversion */
}

/*
 * PRTimeParamFn --
 *
 * A function of PRTimeParamFn type returns the time zone and
 * daylight saving time corrections for some geographic location,
 * given the current time in GMT.  The input argument gmt should
 * point to a PRExplodedTime that is in GMT, i.e., whose
 * tm_params contains all 0's.
 *
 * For any time zone other than GMT, the computation is intended to
 * consist of two steps:
 * - Figure out the time zone correction, tp_gmt_offset.  This number
 * usually depends on the geographic location only.  But it may
 * also depend on the current time.  For example, all of China
 * is one time zone right now.  But this situation may change
 * in the future.
 * - Figure out the daylight saving time correction, tp_dst_offset.
 * This number depends on both the geographic location and the
 * current time.  Most of the DST rules are expressed in local
 * current time.  If so, one should apply the time zone correction
 * to GMT before applying the DST rules.
 */

// typedef PRTimeParameters (PR_CALLBACK *PRTimeParamFn)(const PRExplodedTime *gmt);
pub type PRTimeParamFn = extern "C" fn(gmt: &PRExplodedTime) -> PRTimeParameters;

/********************************************************************* */
/****************************** FUNCTIONS **************************** */
/********************************************************************* */

/*
 * The PR_Now routine returns the current time relative to the
 * epoch, midnight, January 1, 1970 UTC. The units of the returned
 * value are microseconds since the epoch.
 *
 * The values returned are not guaranteed to advance in a linear fashion
 * due to the application of time correction protocols which synchronize
 * computer clocks to some external time source. Consequently it should
 * not be depended on for interval timing.
 *
 * The implementation is machine dependent.
 * Cf. time_t time(time_t *tp) in ANSI C.
 */
// NSPR_API(PRTime)
// PR_Now(void);
#[no_mangle]
pub extern "C" fn PR_Now() -> PRTime {
    eprintln!("PR_Now");
    PRTime::try_from(
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("now is later than epoch")
            .as_micros(),
    )
    .unwrap_or(PRTime::MAX)
}

/*
 * Expand time binding it to time parameters provided by PRTimeParamFn.
 * The calculation is envisoned to proceed in the following steps:
 * - From given PRTime, calculate PRExplodedTime in GMT
 * - Apply the given PRTimeParamFn to the GMT that we just calculated
 * to obtain PRTimeParameters.
 * - Add the PRTimeParameters offsets to GMT to get the local time
 * as PRExplodedTime.
 */

// NSPR_API(void) PR_ExplodeTime(
//     PRTime usecs, PRTimeParamFn params, PRExplodedTime *exploded);
#[no_mangle]
pub extern "C" fn PR_ExplodeTime(
    usecs: PRTime,
    params: PRTimeParamFn,
    exploded: &mut PRExplodedTime,
) {
    unimplemented!()
}

/* Reverse operation of PR_ExplodeTime */
// NSPR_API(PRTime)
// PR_ImplodeTime(const PRExplodedTime *exploded);
#[no_mangle]
pub extern "C" fn PR_ImplodeTime(exploded: &PRExplodedTime) -> PRTime {
    unimplemented!()
}

/*
 * Adjust exploded time to normalize field overflows after manipulation.
 * Note that the following fields of PRExplodedTime should not be
 * manipulated:
 * - tm_month and tm_year: because the number of days in a month and
 * number of days in a year are not constant, it is ambiguous to
 * manipulate the month and year fields, although one may be tempted
 * to.  For example, what does "a month from January 31st" mean?
 * - tm_wday and tm_yday: these fields are calculated by NSPR.  Users
 * should treat them as "read-only".
 */

// NSPR_API(void) PR_NormalizeTime(
//     PRExplodedTime *exploded, PRTimeParamFn params);
#[no_mangle]
pub extern "C" fn PR_NormalizeTime(exploded: &mut PRExplodedTime, params: PRTimeParamFn) {
    unimplemented!()
}

/********************************************************************* */
/*********************** TIME PARAMETER FUNCTIONS ******************** */
/********************************************************************* */

/* Time parameters that suit current host machine */
// NSPR_API(PRTimeParameters) PR_LocalTimeParameters(const PRExplodedTime *gmt);
#[no_mangle]
pub extern "C" fn PR_LocalTimeParameters(gmt: &PRExplodedTime) -> PRTimeParameters {
    unimplemented!()
}

/* Time parameters that represent Greenwich Mean Time */
// NSPR_API(PRTimeParameters) PR_GMTParameters(const PRExplodedTime *gmt);
#[no_mangle]
pub extern "C" fn PR_GMTParameters(gmt: &PRExplodedTime) -> PRTimeParameters {
    unimplemented!()
}

// /*
//  * Time parameters that represent the US Pacific Time Zone, with the
//  * current daylight saving time rules (for testing only)
//  */
// NSPR_API(PRTimeParameters) PR_USPacificTimeParameters(const PRExplodedTime *gmt);
//
// /*
//  * This parses a time/date string into a PRExplodedTime
//  * struct. It populates all fields but it can't split
//  * the offset from UTC into tp_gmt_offset and tp_dst_offset in
//  * most cases (exceptions: PST/PDT, MST/MDT, CST/CDT, EST/EDT, GMT/BST).
//  * In those cases tp_gmt_offset will be the sum of these two and
//  * tp_dst_offset will be 0.
//  * It returns PR_SUCCESS on success, and PR_FAILURE
//  * if the time/date string can't be parsed.
//  *
//  * Many formats are handled, including:
//  *
//  * 14 Apr 89 03:20:12
//  * 14 Apr 89 03:20 GMT
//  * Fri, 17 Mar 89 4:01:33
//  * Fri, 17 Mar 89 4:01 GMT
//  * Mon Jan 16 16:12 PDT 1989
//  * Mon Jan 16 16:12 +0130 1989
//  * 6 May 1992 16:41-JST (Wednesday)
//  * 22-AUG-1993 10:59:12.82
//  * 22-AUG-1993 10:59pm
//  * 22-AUG-1993 12:59am
//  * 22-AUG-1993 12:59 PM
//  * Friday, August 04, 1995 3:54 PM
//  * 06/21/95 04:24:34 PM
//  * 20/06/95 21:07
//  * 95-06-08 19:32:48 EDT
//  *
//  * If the input string doesn't contain a description of the timezone,
//  * we consult the `default_to_gmt' to decide whether the string should
//  * be interpreted relative to the local time zone (PR_FALSE) or GMT (PR_TRUE).
//  * The correct value for this argument depends on what standard specified
//  * the time string which you are parsing.
//  */
//
// NSPR_API(PRStatus) PR_ParseTimeStringToExplodedTime (
//     const char *string,
//     PRBool default_to_gmt,
//     PRExplodedTime *result);

/*
 * This uses PR_ParseTimeStringToExplodedTime to parse
 * a time/date string and PR_ImplodeTime to transform it into
 * a PRTime (microseconds after "1-Jan-1970 00:00:00 GMT").
 * It returns PR_SUCCESS on success, and PR_FAILURE
 * if the time/date string can't be parsed.
 */

// NSPR_API(PRStatus) PR_ParseTimeString (
//     const char *string,
//     PRBool default_to_gmt,
//     PRTime *result);
#[no_mangle]
pub extern "C" fn PR_ParseTimeString(
    string: *const ffi::c_char,
    default_to_gmt: PRBool,
    result: &mut PRTime,
) -> PRStatus {
    unimplemented!()
}

/* Format a time value into a buffer. Same semantics as strftime() */
// NSPR_API(PRUint32) PR_FormatTime(char *buf, int buflen, const char *fmt,
//                                  const PRExplodedTime *time);
#[no_mangle]
pub extern "C" fn PR_FormatTime(
    buf: *mut ffi::c_char,
    buflen: ffi::c_int,
    fmt: *const ffi::c_char,
    time: &PRExplodedTime,
) -> PRUint32 {
    unimplemented!()
}

/* Format a time value into a buffer. Time is always in US English format,
 * regardless of locale setting.
 */
// NSPR_API(PRUint32)
// PR_FormatTimeUSEnglish(char *buf, PRUint32 bufSize,
//                        const char *format, const PRExplodedTime *time);
#[no_mangle]
pub extern "C" fn PR_FormatTimeUSEnglish(
    buf: *mut ffi::c_char,
    bufSize: PRUint32,
    format: *const ffi::c_char,
    time: &PRExplodedTime,
) -> PRUint32 {
    unimplemented!()
}
