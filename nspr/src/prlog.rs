/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! prlog -- Declare interfaces to NSPR's Logging service
//!
//! NSPR provides a logging service that is used by NSPR itself and is
//! available to client programs.
//!
//! To use the service from a client program, you should create a
//! `PRLogModuleInfo` structure by calling `PR_NewLogModule()`. After
//! creating the `LogModule`, you can write to the log using the `PR_LOG()`
//! macro.
//!
//! Initialization of the log service is handled by NSPR initialization.
//!
//! At execution time, you must enable the log service. To enable the
//! log service, set the environment variable: `NSPR_LOG_MODULES`
//! variable.
//!
//! `NSPR_LOG_MODULES` variable has the form:
//!
//!     <moduleName>:<value>[, <moduleName>:<value>]*
//!
//! Where:
//!  <moduleName> is the name passed to `PR_NewLogModule()`.
//!  <value> is a numeric constant, e.g. 5. This value is the maximum
//! value of a log event, enumerated by `PRLogModuleLevel`, that you want
//! written to the log.
//!
//! For example: to record all events of greater value than or equal to
//! `PR_LOG_ERROR` for a `LogModule` names "gizmo", say:
//!
//! set `NSPR_LOG_MODULES=gizmo:2`
//!
//! Note that you must specify the numeric value of `PR_LOG_ERROR`.
//!
//! Special `LogModule` names are provided for controlling NSPR's log
//! service at execution time. These controls should be set in the
//! `NSPR_LOG_MODULES` environment variable at execution time to affect
//! NSPR's log service for your application.
//!
//! The special `LogModule` "all" enables all `LogModules`. To enable all
//! `LogModule` calls to `PR_LOG()`, say:
//!
//! set `NSPR_LOG_MODULES=all:5`
//!
//! The special `LogModule` name "sync" tells the NSPR log service to do
//! unbuffered logging.
//!
//! The special `LogModule` name "bufsize:<size>" tells NSPR to set the
//! log buffer to <size>.
//!
//! The environment variable `NSPR_LOG_FILE` specifies the log file to use
//! unless the default of "stderr" is acceptable. For MS Windows
//! systems, `NSPR_LOG_FILE` can be set to a special value: "`WinDebug`"
//! (case sensitive). This value causes `PR_LOG()` output to be written
//! using the Windows API `OutputDebugString()`. `OutputDebugString()`
//! writes to the debugger window; some people find this helpful.
//!
//!
//! To put log messages in your programs, use the `PR_LOG` macro:
//!
//!     PR_LOG(<module>, <level>, (<printfString>, <args>*));
//!
//! Where <module> is the address of a `PRLogModuleInfo` structure, and
//! <level> is one of the levels defined by the enumeration:
//! `PRLogModuleLevel`. <args> is a `printf()` style of argument list. That
//! is: (fmtstring, ...).
//!
//! Example:
//! ```
//! main() {
//!    PRIntn one = 1;
//!    PRLogModuleInfo * myLm = PR_NewLogModule("gizmo");
//!    PR_LOG( myLm, PR_LOG_ALWAYS, ("Log this! %d\n", one));
//!    return;
//! }
//! ```
//! Note the use of `printf()` style arguments as the third agrument(s) to
//! `PR_LOG()`.
//!
//! After compiling and linking you application, set the environment:
//!
//! set `NSPR_LOG_MODULES=gizmo:5`
//! set `NSPR_LOG_FILE=logfile.txt`
//!
//! When you execute your application, the string "Log this! 1" will be
//! written to the file "logfile.txt".
//!
//! Note to NSPR engineers: a number of `PRLogModuleInfo` structures are
//! defined and initialized in prinit.c. See this module for ideas on
//! what to log where.

use std::ffi::{self, CStr};

use crate::prtypes::PRIntn;

#[repr(C)]
pub enum PRLogModuleLevel {
    PR_LOG_NONE = 0,    /* nothing */
    PR_LOG_ALWAYS = 1,  /* always printed */
    PR_LOG_ERROR = 2,   /* error messages */
    PR_LOG_WARNING = 3, /* warning messages */
    PR_LOG_DEBUG = 4,   /* debug messages */

                        // PR_LOG_NOTICE = PR_LOG_DEBUG,   /* notice messages */
                        // PR_LOG_WARN = PR_LOG_WARNING,   /* warning messages */
                        // PR_LOG_MIN = PR_LOG_DEBUG,      /* minimal debugging messages */
                        // PR_LOG_MAX = PR_LOG_DEBUG       /* maximal debugging messages */
}

/*
 * One of these structures is created for each module that uses logging.
 *    "name" is the name of the module
 *    "level" is the debugging level selected for that module
 */
#[repr(C)]
pub struct PRLogModuleInfo {
    //     const char *name;
    level: PRLogModuleLevel,
    //     struct PRLogModuleInfo *next;
}

/*
 * Create a new log module.
 */
// NSPR_API(PRLogModuleInfo*) PR_NewLogModule(const char *name);
#[no_mangle]
pub extern "C" fn PR_NewLogModule(name: *const ffi::c_char) -> *mut PRLogModuleInfo {
    unimplemented!()
}

// /*
// ** Set the file to use for logging. Returns PR_FALSE if the file cannot
// ** be created
// */
// NSPR_API(PRBool) PR_SetLogFile(const char *name);
//
// /*
// ** Set the size of the logging buffer. If "buffer_size" is zero then the
// ** logging becomes "synchronous" (or unbuffered).
// */
// NSPR_API(void) PR_SetLogBuffering(PRIntn buffer_size);
//
// /*
// ** Print a string to the log. "fmt" is a PR_snprintf format type. All
// ** messages printed to the log are preceeded by the name of the thread
// ** and a time stamp. Also, the routine provides a missing newline if one
// ** is not provided.
// */
// NSPR_API(void) PR_LogPrint(const char *fmt, ...);
//
#[no_mangle]
pub unsafe extern "C" fn PR_LogPrintRust(msg: *const ffi::c_char) {
    let msg = CStr::from_ptr(msg).to_str().expect("valid message");
    println!("XXX {msg}");
    log::error!("YYY {msg}");
}

// /*
// ** Flush the log to its file.
// */
// NSPR_API(void) PR_LogFlush(void);

#[no_mangle]
pub extern "C" fn PR_Assert(s: *const ffi::c_char, file: *const ffi::c_char, ln: PRIntn) {
    let s = unsafe { CStr::from_ptr(s).to_str().expect("valid message") };
    let file = unsafe { CStr::from_ptr(file).to_str().expect("valid file") };
    let msg = format!("Assertion failure: {s}, at {file}:{ln}");
    log::error!("AAA {msg}");
    panic!("BBB {msg}");
}
