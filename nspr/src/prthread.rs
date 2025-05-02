/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! API for NSPR threads. On some architectures (Mac OS Classic
//! notably) pre-emptibility is not guaranteed. Hard priority scheduling
//! is not guaranteed, so programming using priority based synchronization
//! is a no-no.
//!
//! NSPR threads are scheduled based loosely on their client set priority.
//! In general, a thread of a higher priority has a statistically better
//! chance of running relative to threads of lower priority. However,
//! NSPR uses multiple strategies to provide execution vehicles for thread
//! abstraction of various host platforms. As it turns out, there is little
//! NSPR can do to affect the scheduling attributes of "GLOBAL" threads.
//! However, a semblance of GLOBAL threads is used to implement "LOCAL"
//! threads. An arbitrary number of such LOCAL threads can be assigned to
//! a single GLOBAL thread.
//!
//! For scheduling, NSPR will attempt to run the highest priority LOCAL
//! thread associated with a given GLOBAL thread. It is further assumed
//! that the host OS will apply some form of "fair" scheduling on the
//! GLOBAL threads.
//!
//! Threads have a "system flag" which when set indicates the thread
//! doesn't count for determining when the process should exit (the
//! process exits when the last user thread exits).
//!
//! Threads also have a "scope flag" which controls whether the threads
//! are scheduled in the local scope or scheduled by the OS globally. This
//! indicates whether a thread is permanently bound to a native OS thread.
//! An unbound thread competes for scheduling resources in the same process.
//!
//! Another flag is "state flag" which control whether the thread is joinable.
//! It allows other threads to wait for the created thread to reach completion.
//!
//! Threads can have "per-thread-data" attached to them. Each thread has a
//! per-thread error number and error string which are updated when NSPR
//! operations fail.

use std::{
    array,
    cell::RefCell,
    ffi::{self, CStr, CString},
    os::unix::thread::JoinHandleExt,
    ptr,
    thread::{self, JoinHandle, Thread},
};

use crate::{
    prerr::PR_TPD_RANGE_ERROR,
    prerror::PR_SetError,
    prinrval::PRIntervalTime,
    prtypes::{PRInt64, PRStatus, PRUint32, PRUint64, PRUintn},
};

pub struct PRThread(Thread);

#[repr(C)]
pub enum PRThreadType {
    PR_USER_THREAD,
    PR_SYSTEM_THREAD,
}

#[repr(C)]
pub enum PRThreadScope {
    PR_LOCAL_THREAD,
    PR_GLOBAL_THREAD,
    PR_GLOBAL_BOUND_THREAD,
}

#[repr(C)]
pub enum PRThreadState {
    PR_JOINABLE_THREAD,
    PR_UNJOINABLE_THREAD,
}

#[repr(C)]
pub enum PRThreadPriority {
    // PR_PRIORITY_FIRST = 0,      /* just a placeholder */
    PR_PRIORITY_LOW = 0,    /* the lowest possible priority */
    PR_PRIORITY_NORMAL = 1, /* most common expected priority */
    PR_PRIORITY_HIGH = 2,   /* slightly more aggressive scheduling */
    PR_PRIORITY_URGENT = 3, /* it does little good to have more than one */
}

pub const PR_PRIORITY_LAST: PRThreadPriority = PRThreadPriority::PR_PRIORITY_URGENT; /* this is just a placeholder */

#[cfg(not(windows))]
pub fn get_thread_id(handle: Option<&JoinHandle<()>>) -> u64 {
    let mut thread_id: u64 = 0;
    let thread = handle.map_or_else(
        || unsafe { libc::pthread_self() },
        JoinHandleExt::as_pthread_t,
    );
    unsafe {
        libc::pthread_threadid_np(thread, &raw mut thread_id);
    }
    thread_id
}

#[repr(transparent)]
pub struct CreateThreadArg(*mut ffi::c_void);

unsafe impl Send for CreateThreadArg {}

/// Create a new thread:
///     "type" is the type of thread to create
///     "start(arg)" will be invoked as the threads "main"
///     "priority" will be created thread's priority
///     "scope" will specify whether the thread is local or global
///     "state" will specify whether the thread is joinable or not
///     "stackSize" the size of the stack, in bytes. The value can be zero
///        and then a machine specific stack size will be chosen.
///
/// This can return NULL if some kind of error occurs, such as if memory is
/// tight.
///
/// If you want the thread to start up waiting for the creator to do
/// something, enter a lock before creating the thread and then have the
/// threads start routine enter and exit the same lock. When you are ready
/// for the thread to run, exit the lock.
///
/// If you want to detect the completion of the created thread, the thread
/// should be created joinable.  Then, use `PR_JoinThread` to synchrnoize the
/// termination of another thread.
///
/// When the start function returns the thread exits. If it is the last
/// `PR_USER_THREAD` to exit then the process exits.
#[no_mangle]
pub extern "C" fn PR_CreateThread(
    _type: PRThreadType,
    start: extern "C" fn(arg: CreateThreadArg),
    arg: CreateThreadArg,
    _priority: PRThreadPriority,
    _scope: PRThreadScope,
    _state: PRThreadState,
    stackSize: PRUint32,
) -> *mut PRThread {
    eprintln!("PR_CreateThread {stackSize} {:?}", !arg.0.is_null());
    let Ok(stack_size) = usize::try_from(stackSize) else {
        return ptr::null_mut();
    };
    let mut builder = thread::Builder::new();
    if (stack_size > 0) {
        builder = builder.stack_size(stack_size);
    }
    let handler = builder.spawn(move || start(arg));
    let Ok(handler) = handler else {
        return ptr::null_mut();
    };
    let x = get_thread_id(Some(&handler));
    eprintln!("PR_CreateThread: {x:?} {:?}", handler.thread().id());
    x as *mut PRThread
}

/*
 * Wait for thread termination:
 *     "thread" is the target thread
 *
 * This can return PR_FAILURE if no joinable thread could be found
 * corresponding to the specified target thread.
 *
 * The calling thread is blocked until the target thread completes.
 * Several threads cannot wait for the same thread to complete; one thread
 * will operate successfully and others will terminate with an error PR_FAILURE.
 * The calling thread will not be blocked if the target thread has already
 * terminated.
 */
// NSPR_API(PRStatus) PR_JoinThread(PRThread *thread);
#[no_mangle]
pub extern "C" fn PR_JoinThread(thread: *mut PRThread) -> PRStatus {
    unimplemented!()
}

/// Return the current thread object for the currently running code.
/// Never returns NULL.
#[no_mangle]
pub extern "C" fn PR_GetCurrentThread() -> *mut PRThread {
    let x = get_thread_id(None);
    // eprintln!("PR_GetCurrentThread: {x:?}");
    x as *mut PRThread
}

// #ifndef NO_NSPR_10_SUPPORT
// #define PR_CurrentThread() PR_GetCurrentThread() /* for nspr1.0 compat. */
// #endif /* NO_NSPR_10_SUPPORT */
//
// /*
// ** Get the priority of "thread".
// */
// NSPR_API(PRThreadPriority) PR_GetThreadPriority(const PRThread *thread);

/*
 * Change the priority of the "thread" to "priority".
 *
 * PR_SetThreadPriority works in a best-effort manner. On some platforms a
 * special privilege, such as root access, is required to change thread
 * priorities, especially to raise thread priorities. If the caller doesn't
 * have enough privileges to change thread priorites, the function has no
 * effect except causing a future PR_GetThreadPriority call to return
 * |priority|.
 */
// NSPR_API(void) PR_SetThreadPriority(PRThread *thread, PRThreadPriority priority);
#[no_mangle]
pub extern "C" fn PR_SetThreadPriority(thread: *mut PRThread, priority: PRThreadPriority) {
    unimplemented!()
}

thread_local! {
    static THREAD_NAME: RefCell<String> = const { RefCell::new(String::new()) };
}

/// Set the name of the current thread, which will be visible in a debugger
/// and accessible via a call to `PR_GetThreadName()`.
#[no_mangle]
pub extern "C" fn PR_SetCurrentThreadName(name: *const ffi::c_char) -> PRStatus {
    debug_assert!(!name.is_null());
    THREAD_NAME.with(|thread_name| {
        let name = unsafe { CStr::from_ptr(name) };
        let Ok(name) = name.to_str() else {
            return PRStatus::PR_FAILURE;
        };
        thread_name.borrow_mut().replace_range(.., name);
        eprintln!("PR_SetCurrentThreadName {}", thread_name.borrow());
        PRStatus::PR_SUCCESS
    })
}

/// Return the name of "thread", if set.  Otherwise return NULL.
#[no_mangle]
pub extern "C" fn PR_GetThreadName(thread: *const PRThread) -> *const ffi::c_char {
    eprintln!("PR_GetThreadName");
    debug_assert!(!thread.is_null());
    let (thread) = unsafe { &*thread.cast::<Thread>() };
    let Some(name) = thread.name() else {
        return ptr::null();
    };
    let Ok(name) = CString::new(name) else {
        return ptr::null();
    };
    name.into_raw()
}

#[no_mangle]
pub type PRThreadPrivateDTOR = extern "C" fn(priv_: *mut ffi::c_void);

#[derive(Default)]
struct ThreadLocalEntry {
    data: *mut ffi::c_void,
    dtor: Option<PRThreadPrivateDTOR>,
}

impl Drop for ThreadLocalEntry {
    fn drop(&mut self) {
        // eprintln!("Drop for ThreadLocalEntry");
        self.drop();
    }
}

impl ThreadLocalEntry {
    fn drop(&mut self) {
        if self.data.is_null() {
            return;
        }
        let Some(dtor) = self.dtor else {
            return;
        };
        eprintln!("ThreadLocalEntry::drop");
        unsafe { dtor(self.data) }
        self.data = ptr::null_mut();
    }
}

struct ThreadLocalData {
    entry_map: u128,
    entries: [ThreadLocalEntry; u128::BITS as usize],
}

impl Default for ThreadLocalData {
    fn default() -> Self {
        Self {
            entry_map: 0,
            entries: array::from_fn(|_| ThreadLocalEntry::default()),
        }
    }
}

thread_local! {
    static THREAD_PRIVATE_DATA: RefCell<ThreadLocalData> = RefCell::new(ThreadLocalData::default());
}

/// This routine returns a new index for per-thread-private data table.
/// The index is visible to all threads within a process. This index can
/// be used with the `PR_SetThreadPrivate()` an`PR_GetThreadPrivate()`() routines
/// to save and retrieve data associated with the index for a thread.
///
/// Each index is associationed with a destructor function ('dtor'). The function
/// may be specified as NULL when the index is created. If it is not NULL, the
/// function will be called when:
///      - the thread exits and the private data for the associated index is not NULL,
///      - new thread private data is set and the current private data is not NULL.
///
/// The index independently maintains specific values for each binding thread.
/// A thread can only get access to its own thread-specific-data.
///
/// Upon a new index return the value associated with the index for all threads
/// is NULL, and upon thread creation the value associated with all indices for
/// that thread is NULL.
///
/// Returns `PR_FAILURE` if the total number of indices will exceed the maximun
/// allowed.
#[no_mangle]
pub extern "C" fn PR_NewThreadPrivateIndex(
    newIndex: *mut PRUintn,
    destructor: PRThreadPrivateDTOR,
) -> PRStatus {
    eprintln!("PR_NewThreadPrivateIndex");
    debug_assert!(!newIndex.is_null());
    THREAD_PRIVATE_DATA.with_borrow_mut(|data| {
        // Find the first empty slot in the thread private data table.
        let index = data.entry_map.trailing_ones();
        if index >= u128::BITS {
            // No empty slot found, return failure.
            PR_SetError(PR_TPD_RANGE_ERROR, 0);
            return PRStatus::PR_FAILURE;
        }
        let Ok(idx) = usize::try_from(index) else {
            PR_SetError(PR_TPD_RANGE_ERROR, 0);
            return PRStatus::PR_FAILURE;
        };
        data.entry_map |= 1 << index;
        data.entries[idx].dtor = Some(destructor);
        unsafe {
            *newIndex = index;
        }
        eprintln!(
            "PR_NewThreadPrivateIndex: {index} {}",
            data.entries[idx].dtor.is_some()
        );
        PRStatus::PR_SUCCESS
    })
}

/// Define some per-thread-private data.
///     "tpdIndex" is an index into the per-thread private data table
///     "priv" is the per-thread-private data
///
/// If the per-thread private data table has a previously registered
/// destructor function and a non-NULL per-thread-private data value,
/// the destructor function is invoked.
///
/// This can return `PR_FAILURE` if the index is invalid.
#[no_mangle]
pub extern "C" fn PR_SetThreadPrivate(tpdIndex: PRUintn, private: *mut ffi::c_void) -> PRStatus {
    eprintln!("PR_SetThreadPrivate {tpdIndex}");
    let Ok(index) = usize::try_from(tpdIndex) else {
        PR_SetError(PR_TPD_RANGE_ERROR, 0);
        return PRStatus::PR_FAILURE;
    };
    THREAD_PRIVATE_DATA.with_borrow_mut(|data| {
        let Some(entry) = data.entries.get_mut(index) else {
            PR_SetError(PR_TPD_RANGE_ERROR, 0);
            return PRStatus::PR_FAILURE;
        };
        entry.drop();
        entry.data = private;
        PRStatus::PR_SUCCESS
    })
}

/// Recover the per-thread-private data for the current thread. "tpdIndex" is
/// the index into the per-thread private data table.
///
/// The returned value may be NULL which is indistinguishable from an error
/// condition.
///
/// A thread can only get access to its own thread-specific-data.
#[no_mangle]
pub extern "C" fn PR_GetThreadPrivate(tpdIndex: PRUintn) -> *mut ffi::c_void {
    // eprintln!("PR_GetThreadPrivate {tpdIndex}");
    let Ok(index) = usize::try_from(tpdIndex) else {
        return ptr::null_mut();
    };
    THREAD_PRIVATE_DATA.with_borrow(|data| {
        data.entries
            .get(index)
            .map_or_else(ptr::null_mut, |entry| entry.data)
    })
}

/*
 * This routine sets the interrupt request for a target thread. The interrupt
 * request remains in the thread's state until it is delivered exactly once
 * or explicitly canceled.
 *
 * A thread that has been interrupted will fail all NSPR blocking operations
 * that return a PRStatus (I/O, waiting on a condition, etc).
 *
 * PR_Interrupt may itself fail if the target thread is invalid.
 */
// NSPR_API(PRStatus) PR_Interrupt(PRThread *thread);
#[no_mangle]
pub extern "C" fn PR_Interrupt(thread: *mut PRThread) -> PRStatus {
    unimplemented!()
}

// /*
// ** Clear the interrupt request for the calling thread. If no such request
// ** is pending, this operation is a noop.
// */
// NSPR_API(void) PR_ClearInterrupt(void);
//
// /*
// ** Block the interrupt for the calling thread.
// */
// NSPR_API(void) PR_BlockInterrupt(void);
//
// /*
// ** Unblock the interrupt for the calling thread.
// */
// NSPR_API(void) PR_UnblockInterrupt(void);

/*
 * Make the current thread sleep until "ticks" time amount of time
 * has expired. If "ticks" is PR_INTERVAL_NO_WAIT then the call is
 * equivalent to calling PR_Yield. Calling PR_Sleep with an argument
 * equivalent to PR_INTERVAL_NO_TIMEOUT is an error and will result
 * in a PR_FAILURE error return.
 */
// NSPR_API(PRStatus) PR_Sleep(PRIntervalTime ticks);
#[no_mangle]
pub extern "C" fn PR_Sleep(ticks: PRIntervalTime) -> PRStatus {
    unimplemented!()
}

// /*
// ** Get the scoping of this thread.
// */
// NSPR_API(PRThreadScope) PR_GetThreadScope(const PRThread *thread);
//
// /*
// ** Get the type of this thread.
// */
// NSPR_API(PRThreadType) PR_GetThreadType(const PRThread *thread);
//
// /*
// ** Get the join state of this thread.
// */
// NSPR_API(PRThreadState) PR_GetThreadState(const PRThread *thread);
//
// PR_END_EXTERN_C
//
// #endif /* prthread_h___ */
