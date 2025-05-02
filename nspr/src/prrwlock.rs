/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::ffi;

use crate::prtypes::PRUint32;

/*
 * File:        prrwlock.h
 * Description: API to basic reader-writer lock functions of NSPR.
 *
 */

/*
 * PRRWLock --
 *
 *  The reader writer lock, PRRWLock, is an opaque object to the clients
 *  of NSPR.  All routines operate on a pointer to this opaque entity.
 */

#[repr(C)]
pub struct PRRWLock {}

// #define PR_RWLOCK_RANK_NONE 0
pub const PR_RWLOCK_RANK_NONE: u32 = 0;

/***********************************************************************
 * FUNCTION:    PR_NewRWLock
 * DESCRIPTION:
 *  Returns a pointer to a newly created reader-writer lock object.
 * INPUTS:      Lock rank
 *              Lock name
 * OUTPUTS:     void
 * RETURN:      PRRWLock*
 *   If the lock cannot be created because of resource constraints, NULL
 *   is returned.
 *
 ********************************************************************* */
// NSPR_API(PRRWLock*) PR_NewRWLock(PRUint32 lock_rank, const char *lock_name);
#[no_mangle]
pub extern "C" fn PR_NewRWLock(
    lock_rank: PRUint32,
    lock_name: *const ffi::c_char,
) -> *mut PRRWLock {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:    PR_DestroyRWLock
 * DESCRIPTION:
 *  Destroys a given RW lock object.
 * INPUTS:      PRRWLock *lock - Lock to be freed.
 * OUTPUTS:     void
 * RETURN:      None
 ********************************************************************* */
// NSPR_API(void) PR_DestroyRWLock(PRRWLock *lock);
#[no_mangle]
pub extern "C" fn PR_DestroyRWLock(lock: *mut PRRWLock) {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:    PR_RWLock_Rlock
 * DESCRIPTION:
 *  Apply a read lock (non-exclusive) on a RWLock
 * INPUTS:      PRRWLock *lock - Lock to be read-locked.
 * OUTPUTS:     void
 * RETURN:      None
 ********************************************************************* */
// NSPR_API(void) PR_RWLock_Rlock(PRRWLock *lock);
#[no_mangle]
pub extern "C" fn PR_RWLock_Rlock(lock: *mut PRRWLock) {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:    PR_RWLock_Wlock
 * DESCRIPTION:
 *  Apply a write lock (exclusive) on a RWLock
 * INPUTS:      PRRWLock *lock - Lock to write-locked.
 * OUTPUTS:     void
 * RETURN:      None
 ********************************************************************* */
// NSPR_API(void) PR_RWLock_Wlock(PRRWLock *lock);
#[no_mangle]
pub extern "C" fn PR_RWLock_Wlock(lock: *mut PRRWLock) {
    unimplemented!()
}

/***********************************************************************
 * FUNCTION:    PR_RWLock_Unlock
 * DESCRIPTION:
 *  Release a RW lock. Unlocking an unlocked lock has undefined results.
 * INPUTS:      PRRWLock *lock - Lock to unlocked.
 * OUTPUTS:     void
 * RETURN:      void
 ********************************************************************* */
// NSPR_API(void) PR_RWLock_Unlock(PRRWLock *lock);
#[no_mangle]
pub extern "C" fn PR_RWLock_Unlock(lock: *mut PRRWLock) {
    unimplemented!()
}
