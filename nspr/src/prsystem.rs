/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! API to NSPR functions returning system info.

use std::{ffi, sync::LazyLock};

use static_assertions::const_assert;

// use sysinfo::{MemoryRefreshKind, RefreshKind, System};
use crate::prtypes::{PRInt32, PRIntn, PRStatus, PRUint32, PRUint64};

// const_assert!(sysinfo::IS_SUPPORTED_SYSTEM);

// static SYSINFO: LazyLock<System> = LazyLock::new();

// fn init_sysinfo() -> System {
//     let mut sys = System::new();
//     sys.refresh_memory_specifics(MemoryRefreshKind::nothing().with_ram());
//     sys
// }

/*
 * Get the host' directory separator.
 *  Pathnames are then assumed to be of the form:
 *      [<sep><root_component><sep>]*(<component><sep>)<leaf_name>
 */

// NSPR_API(char) PR_GetDirectorySeparator(void);
#[no_mangle]
pub extern "C" fn PR_GetDirectorySeparator() -> ffi::c_char {
    unimplemented!()
}

//
// /*
// ** OBSOLETE -- the function name is misspelled.
// ** Use PR_GetDirectorySeparator instead.
// */
//
// NSPR_API(char) PR_GetDirectorySepartor(void);
//
// /*
// ** Get the host' path separator.
// **  Paths are assumed to be of the form:
// **      <directory>[<sep><directory>]*
// */
//
// NSPR_API(char) PR_GetPathSeparator(void);

/* Types of information available via PR_GetSystemInfo(...) */
#[repr(C)]
pub enum PRSysInfo {
    PR_SI_HOSTNAME, /* the hostname with the domain name (if any)
                     * removed */
    PR_SI_SYSNAME,
    PR_SI_RELEASE,
    PR_SI_ARCHITECTURE,
    PR_SI_HOSTNAME_UNTRUNCATED, /* the hostname exactly as configured
                                 * on the system */
    PR_SI_RELEASE_BUILD,
}

/*
 * If successful returns a null termintated string in 'buf' for
 * the information indicated in 'cmd'. If unseccussful the reason for
 * the failure can be retrieved from PR_GetError().
 *
 * The buffer is allocated by the caller and should be at least
 * SYS_INFO_BUFFER_LENGTH bytes in length.
 */

// #define SYS_INFO_BUFFER_LENGTH 256
pub const SYS_INFO_BUFFER_LENGTH: PRIntn = 256;

// NSPR_API(PRStatus) PR_GetSystemInfo(PRSysInfo cmd, char *buf, PRUint32 buflen);
#[no_mangle]
pub extern "C" fn PR_GetSystemInfo(
    cmd: PRSysInfo,
    buf: *mut ffi::c_char,
    buflen: PRUint32,
) -> PRStatus {
    unimplemented!()
}

/*
 * Return the number of bytes in a page
 */
// NSPR_API(PRInt32) PR_GetPageSize(void);
#[no_mangle]
pub extern "C" fn PR_GetPageSize() -> PRInt32 {
    unimplemented!()
}

/*
 * Return log2 of the size of a page
 */
// NSPR_API(PRInt32) PR_GetPageShift(void);
#[no_mangle]
pub extern "C" fn PR_GetPageShift() -> PRInt32 {
    unimplemented!()
}

/*
 * PR_GetNumberOfProcessors() -- returns the number of CPUs
 *
 * Description:
 * PR_GetNumberOfProcessors() extracts the number of processors
 * (CPUs available in an SMP system) and returns the number.
 *
 * Parameters:
 *   none
 *
 * Returns:
 *   The number of available processors or -1 on error
 *
 */
// NSPR_API(PRInt32) PR_GetNumberOfProcessors( void );
#[no_mangle]
pub extern "C" fn PR_GetNumberOfProcessors() -> PRInt32 {
    unimplemented!()
}

/*
 * PR_GetPhysicalMemorySize() -- returns the amount of system RAM
 *
 * Description:
 * PR_GetPhysicalMemorySize() determines the amount of physical RAM
 * in the system and returns the size in bytes.
 *
 * Parameters:
 *   none
 *
 * Returns:
 *   The amount of system RAM, or 0 on failure.
 *
 */
// NSPR_API(PRUint64) PR_GetPhysicalMemorySize(void);
#[no_mangle]
pub extern "C" fn PR_GetPhysicalMemorySize() -> PRUint64 {
    // let sys = SYSINFO.get_or_init(init_sysinfo);
    // eprintln!("PR_GetPhysicalMemorySize {}", sys.total_memory());
    // sys.total_memory()
    32 * 1024 * 1024 * 1024 // FIXME
}
