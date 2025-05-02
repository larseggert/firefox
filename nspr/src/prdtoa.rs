/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::ffi;

use crate::prtypes::{PRFloat64, PRIntn, PRSize, PRStatus};

/// `PR_strtod()` returns as a double-precision floating-point number
/// the  value represented by the character string pointed to by
/// s00. The string is scanned up to the first unrecognized
/// character.
///
/// If the value of se is not (char **)NULL, a  pointer  to
/// the  character terminating the scan is returned in the location pointed
/// to by se. If no number can be formed, se is set to s00, and
/// zero is returned.
#[no_mangle]
pub extern "C" fn PR_strtod(s00: *const ffi::c_char, se: *mut *mut ffi::c_char) -> PRFloat64 {
    unsafe { libc::strtod(s00, se) }
}

/*
 * PR_dtoa() converts double to a string.
 *
 * ARGUMENTS:
 * If rve is not null, *rve is set to point to the end of the return value.
 * If d is +-Infinity or NaN, then *decpt is set to 9999.
 *
 * mode:
 *     0 ==> shortest string that yields d when read in
 *           and rounded to nearest.
 */
// NSPR_API(PRStatus) PR_dtoa(PRFloat64 d, PRIntn mode, PRIntn ndigits,
//                            PRIntn *decpt, PRIntn *sign, char **rve, char *buf, PRSize bufsize);
//
#[no_mangle]
pub extern "C" fn PR_dtoa(
    d: PRFloat64,
    mode: PRIntn,
    ndigits: PRIntn,
    decpt: *mut PRIntn,
    sign: *mut PRIntn,
    rve: *mut *mut ffi::c_char,
    buf: *mut ffi::c_char,
    bufsize: PRSize,
) -> PRStatus {
    unimplemented!()
}
