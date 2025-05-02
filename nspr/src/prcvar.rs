/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::{mem, thread, time::Duration};

use parking_lot::Condvar;

use crate::{
    prinrval::{PRIntervalTime, PR_INTERVAL_NO_TIMEOUT},
    prlock::PRLock,
    prtypes::PRStatus,
};

pub struct PRCondVar {
    condvar: Condvar,
    lock: *mut PRLock,
}

/// Create a new condition variable.
///
/// "lock" is the lock used to protect the condition variable.
///
/// Condition variables are synchronization objects that threads can use
/// to wait for some condition to occur.
///
/// This may fail if memory is tight or if some operating system resource
/// is low. In such cases, a NULL will be returned.
#[no_mangle]
pub extern "C" fn PR_NewCondVar(lock: *mut PRLock) -> *mut PRCondVar {
    eprintln!("PR_NewCondVar");
    debug_assert!(!lock.is_null());
    let condvar = Box::new(PRCondVar {
        condvar: Condvar::new(),
        lock,
    });
    Box::into_raw(condvar)
}

/// Destroy a condition variable. There must be no thread
/// waiting on the condvar. The caller is responsible for guaranteeing
/// that the condvar is no longer in use.
#[no_mangle]
pub extern "C" fn PR_DestroyCondVar(cvar: *mut PRCondVar) {
    eprintln!("PR_DestroyCondVar");
    debug_assert!(!cvar.is_null());
    _ = unsafe { Box::from_raw(cvar) };
}

/// The thread that waits on a condition is blocked in a "waiting on
/// condition" state until another thread notifies the condition or a
/// caller specified amount of time expires. The lock associated with
/// the condition variable will be released, which must have be held
/// prior to the call to wait.
///
/// Logically a notified thread is moved from the "waiting on condition"
/// state and made "ready." When scheduled, it will attempt to reacquire
/// the lock that it held when wait was called.
///
/// The timeout has two well known values, `PR_INTERVAL_NO_TIMEOUT` and
/// `PR_INTERVAL_NO_WAIT`. The former value requires that a condition be
/// notified (or the thread interrupted) before it will resume from the
/// wait. If the timeout has a value of `PR_INTERVAL_NO_WAIT`, the effect
/// is to release the lock, possibly causing a rescheduling within the
/// runtime, then immediately attempting to reacquire the lock and resume.
///
/// Any other value for timeout will cause the thread to be rescheduled
/// either due to explicit notification or an expired interval. The latter
/// must be determined by treating time as one part of the monitored data
/// being protected by the lock and tested explicitly for an expired
/// interval.
///
/// Returns `PR_FAILURE` if the caller has not locked the lock associated
/// with the condition variable or the thread was interrupted (`PR_Interrupt()`).
/// The particular reason can be extracted with `PR_GetError()`.
#[no_mangle]
pub extern "C" fn PR_WaitCondVar(cvar: *mut PRCondVar, timeout: PRIntervalTime) -> PRStatus {
    eprintln!("PR_WaitCondVar");
    debug_assert!(!cvar.is_null());
    let mut cvar = unsafe { Box::from_raw(cvar) };
    let mut lock = unsafe { Box::from_raw(cvar.lock) };
    debug_assert!(lock.is_locked()); // We'd better be locked...
    debug_assert_eq!(lock.owner(), Some(thread::current().id())); // ... and it better be by us.
    eprintln!("PR_WaitCondVar lock: {lock:?}");

    //   if (timeout == PR_INTERVAL_NO_TIMEOUT) {
    //     rv = pthread_cond_wait(&cvar->cv, &cvar->lock->mutex);
    //   } else {
    //     rv = pt_TimedWait(&cvar->cv, &cvar->lock->mutex, timeout);
    //   }

    //   /* We just got the lock back - this better be empty */
    //   PR_ASSERT(PR_FALSE == cvar->lock->locked);
    //   cvar->lock->locked = PR_TRUE;
    //   cvar->lock->owner = pthread_self();

    let mut guard = lock.guard();
    let res = if timeout == PR_INTERVAL_NO_TIMEOUT {
        cvar.condvar.wait(&mut guard);
        PRStatus::PR_SUCCESS
    } else if cvar
        .condvar
        .wait_for(&mut guard, Duration::from_micros(timeout))
        .timed_out()
    {
        PRStatus::PR_FAILURE
    } else {
        PRStatus::PR_SUCCESS
    };
    #[expect(
        clippy::mem_forget,
        reason = "We need to keep the lock alive after the guard is dropped."
    )]
    mem::forget(guard);
    Box::into_raw(lock);
    Box::into_raw(cvar);
    res
}

/*
 * Notify ONE thread that is currently waiting on 'cvar'. Which thread is
 * dependent on the implementation of the runtime. Common sense would dictate
 * that all threads waiting on a single condition have identical semantics,
 * therefore which one gets notified is not significant.
 *
 * The calling thead must hold the lock that protects the condition, as
 * well as the invariants that are tightly bound to the condition, when
 * notify is called.
 *
 * Returns PR_FAILURE if the caller has not locked the lock associated
 * with the condition variable.
 */
// NSPR_API(PRStatus) PR_NotifyCondVar(PRCondVar *cvar);
#[no_mangle]
pub extern "C" fn PR_NotifyCondVar(cvar: *mut PRCondVar) -> PRStatus {
    unimplemented!()
}

/*
 * Notify all of the threads waiting on the condition variable. The order
 * that the threads are notified is indeterminant. The lock that protects
 * the condition must be held.
 *
 * Returns PR_FAILURE if the caller has not locked the lock associated
 * with the condition variable.
 */
// NSPR_API(PRStatus) PR_NotifyAllCondVar(PRCondVar *cvar);
#[no_mangle]
pub extern "C" fn PR_NotifyAllCondVar(cvar: *mut PRCondVar) -> PRStatus {
    unimplemented!()
}
