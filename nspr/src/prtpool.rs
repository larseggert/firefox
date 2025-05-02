/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::ffi;

use crate::prtypes::{PRBool, PRInt32, PRStatus, PRUint32};

/*
 * NOTE:
 *      THIS API IS A PRELIMINARY VERSION IN NSPR 4.0 AND IS SUBJECT TO
 *      CHANGE
 */
//
// PR_BEGIN_EXTERN_C
//
// typedef struct PRJobIoDesc {
//     PRFileDesc *socket;
//     PRErrorCode error;
//     PRIntervalTime timeout;
// } PRJobIoDesc;
//
// typedef struct PRThreadPool PRThreadPool;
#[repr(C)]
pub struct PRThreadPool {}

// typedef struct PRJob PRJob;
// typedef void (PR_CALLBACK *PRJobFn) (void *arg);

/* Create thread pool */
// NSPR_API(PRThreadPool *)
// PR_CreateThreadPool(PRInt32 initial_threads, PRInt32 max_threads,
//                     PRUint32 stacksize);
#[no_mangle]
pub extern "C" fn PR_CreateThreadPool(
    initial_threads: PRInt32,
    max_threads: PRInt32,
    stacksize: PRUint32,
) -> *mut PRThreadPool {
    unimplemented!()
}

/* queue a job */
// NSPR_API(PRJob *)
// PR_QueueJob(PRThreadPool *tpool, PRJobFn fn, void *arg, PRBool joinable);
#[no_mangle]
pub extern "C" fn PR_QueueJob(
    tpool: *mut PRThreadPool,
    fn_: extern "C" fn(*mut ffi::c_void),
    arg: *mut ffi::c_void,
    joinable: PRBool,
) -> *mut ffi::c_void {
    unimplemented!()
}

// /* queue a job, when a socket is readable */
// NSPR_API(PRJob *)
// PR_QueueJob_Read(PRThreadPool *tpool, PRJobIoDesc *iod,
//                  PRJobFn fn, void * arg, PRBool joinable);
//
// /* queue a job, when a socket is writeable */
// NSPR_API(PRJob *)
// PR_QueueJob_Write(PRThreadPool *tpool, PRJobIoDesc *iod,
//                   PRJobFn fn, void * arg, PRBool joinable);
//
// /* queue a job, when a socket has a pending connection */
// NSPR_API(PRJob *)
// PR_QueueJob_Accept(PRThreadPool *tpool, PRJobIoDesc *iod,
//                    PRJobFn fn, void * arg, PRBool joinable);
//
// /* queue a job, when the socket connection to addr succeeds or fails */
// NSPR_API(PRJob *)
// PR_QueueJob_Connect(PRThreadPool *tpool, PRJobIoDesc *iod,
//                     const PRNetAddr *addr, PRJobFn fn, void * arg, PRBool joinable);
//
// /* queue a job, when a timer exipres */
// NSPR_API(PRJob *)
// PR_QueueJob_Timer(PRThreadPool *tpool, PRIntervalTime timeout,
//                   PRJobFn fn, void * arg, PRBool joinable);
// /* cancel a job */
// NSPR_API(PRStatus)
// PR_CancelJob(PRJob *job);
//
// /* join a job */
// NSPR_API(PRStatus)
// PR_JoinJob(PRJob *job);
//
/* shutdown pool */
// NSPR_API(PRStatus)
// PR_ShutdownThreadPool(PRThreadPool *tpool);
#[no_mangle]
pub extern "C" fn PR_ShutdownThreadPool(tpool: *mut PRThreadPool) -> PRStatus {
    unimplemented!()
}

/* join pool, wait for exit of all threads */
// NSPR_API(PRStatus)
// PR_JoinThreadPool(PRThreadPool *tpool);
#[no_mangle]
pub extern "C" fn PR_JoinThreadPool(tpool: *mut PRThreadPool) -> PRStatus {
    unimplemented!()
}
