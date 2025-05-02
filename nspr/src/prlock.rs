/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! API to basic locking functions of NSPR.
//!
//! NSPR provides basic locking mechanisms for thread synchronization.  Locks
//! are lightweight resource contention controls that prevent multiple threads
//! from accessing something (code/data) simultaneously.

use std::{
    mem,
    thread::{self, ThreadId},
};

use parking_lot::{Mutex, MutexGuard};

use crate::{prthread::get_thread_id, prtypes::PRStatus};

/// NSPR represents the lock as an opaque entity to the client of the
/// API.  All routines operate on a pointer to this opaque entity.
#[derive(Debug)]
pub struct PRLock {
    mutex: Mutex<()>,
    owner: Option<ThreadId>,
}

impl PRLock {
    pub const fn new() -> Self {
        Self {
            mutex: Mutex::new(()),
            owner: None,
        }
    }

    pub fn is_locked(&self) -> bool {
        self.mutex.is_locked()
    }

    pub fn lock(&mut self) -> MutexGuard<'_, ()> {
        let guard = self.mutex.lock();
        self.owner = Some(thread::current().id());
        guard
    }

    pub fn unlock(&mut self) {
        self.owner = None;
        unsafe { self.mutex.force_unlock() }
    }

    pub const fn owner(&self) -> Option<ThreadId> {
        self.owner
    }

    pub fn guard(&self) -> MutexGuard<'_, ()> {
        debug_assert!(self.is_locked(), "not locked");
        assert_eq!(self.owner, Some(thread::current().id()));
        unsafe { self.mutex.make_guard_unchecked() }
    }
}

/// Returns a pointer to a newly created opaque lock object.
///
/// # Errrors
///
/// If the lock can not be created because of resource constraints, NULL
/// is returned.
#[no_mangle]
pub extern "C" fn PR_NewLock() -> *mut PRLock {
    eprintln!("PR_NewLock");
    Box::into_raw(Box::new(PRLock::new()))
}

/// Destroys a given opaque lock object.
#[no_mangle]
pub extern "C" fn PR_DestroyLock(lock: *mut PRLock) {
    eprintln!("PR_DestroyLock");
    debug_assert!(!lock.is_null());
    let lock = unsafe { Box::from_raw(lock) };
    debug_assert!(!lock.is_locked(), "locked by {:?}", lock.owner);
}

///  Lock a lock.
#[no_mangle]
pub extern "C" fn PR_Lock(lock: *mut PRLock) {
    // eprintln!("PR_Lock");
    debug_assert!(!lock.is_null());
    let mut lock = unsafe { Box::from_raw(lock) };
    #[expect(
        clippy::mem_forget,
        reason = "We need to keep the lock alive after the guard is dropped."
    )]
    mem::forget(lock.lock());
    Box::into_raw(lock);
}

/// Unlock a lock.  Unlocking an unlocked lock has undefined results.
///
/// # Errors
///
/// Returns `PR_FAILURE` if the caller does not own the lock.
#[no_mangle]
pub extern "C" fn PR_Unlock(lock: *mut PRLock) -> PRStatus {
    // eprintln!("PR_Unlock");
    debug_assert!(!lock.is_null());
    let mut lock = unsafe { Box::from_raw(lock) };
    lock.unlock();
    Box::into_raw(lock);
    PRStatus::PR_SUCCESS
}

/// # Panics
///
/// If the current thread owns |lock|, this assertion is guaranteed to
/// succeed.  Otherwise, the behavior of this function is undefined.
///
/// Don't call this function directly.
#[no_mangle]
pub extern "C" fn PR_AssertCurrentThreadOwnsLock(lock: *mut PRLock) {
    let mut lock = unsafe { Box::from_raw(lock) };
    assert_eq!(lock.owner, Some(thread::current().id()));
    Box::into_raw(lock);
}
