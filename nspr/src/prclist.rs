/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// #ifndef prclist_h___
// #define prclist_h___
//
// #include "prtypes.h"
//
// typedef struct PRCListStr PRCList;

use std::ptr;

use crate::prtypes::PRBool;

#[repr(transparent)]
pub struct PRCListStr(PRCList);

/*
 * Circular linked list
 */
#[repr(C)]
pub struct PRCList {
    next: *mut PRCList,
    prev: *mut PRCList,
}

/*
 * Insert element "_e" into the list, before "_l".
 */
// #define PR_INSERT_BEFORE(_e,_l)  \
//     PR_BEGIN_MACRO       \
//     (_e)->next = (_l);   \
//     (_e)->prev = (_l)->prev; \
//     (_l)->prev->next = (_e); \
//     (_l)->prev = (_e);   \
//     PR_END_MACRO

#[no_mangle]
pub extern "C" fn PR_INSERT_BEFORE(e: *mut PRCList, l: *mut PRCList) {
    unsafe {
        (*e).next = l;
        (*e).prev = (*l).prev;
        (*(*l).prev).next = e;
        (*l).prev = e;
    }
}

/*
 * Insert element "_e" into the list, after "_l".
 */
// #define PR_INSERT_AFTER(_e,_l)   \
//     PR_BEGIN_MACRO       \
//     (_e)->next = (_l)->next; \
//     (_e)->prev = (_l);   \
//     (_l)->next->prev = (_e); \
//     (_l)->next = (_e);   \
//     PR_END_MACRO

#[no_mangle]
pub extern "C" fn PR_INSERT_AFTER(e: *mut PRCList, l: *mut PRCList) {
    unsafe {
        (*e).next = (*l).next;
        (*e).prev = l;
        (*(*l).next).prev = e;
        (*l).next = e;
    }
}

/*
 * Return the element following element "_e"
 */
// #define PR_NEXT_LINK(_e)     \
//         ((_e)->next)

#[no_mangle]
pub const extern "C" fn PR_NEXT_LINK(e: *const PRCList) -> *mut PRCList {
    unsafe { (*e).next }
}

/*
 * Return the element preceding element "_e"
 */
// #define PR_PREV_LINK(_e)     \
//         ((_e)->prev)
#[no_mangle]
pub const extern "C" fn PR_PREV_LINK(e: *const PRCList) -> *mut PRCList {
    unsafe { (*e).prev }
}

/*
 * Append an element "_e" to the end of the list "_l"
 */
// #define PR_APPEND_LINK(_e,_l) PR_INSERT_BEFORE(_e,_l)

#[no_mangle]
pub extern "C" fn PR_APPEND_LINK(e: *mut PRCList, l: *mut PRCList) {
    unsafe { PR_INSERT_BEFORE(e, l) }
}

/*
 * Insert an element "_e" at the head of the list "_l"
 */
// #define PR_INSERT_LINK(_e,_l) PR_INSERT_AFTER(_e,_l)
#[no_mangle]
pub extern "C" fn PR_INSERT_LINK(e: *mut PRCList, l: *mut PRCList) {
    unsafe { PR_INSERT_AFTER(e, l) }
}

/* Return the head/tail of the list */
// #define PR_LIST_HEAD(_l) (_l)->next
#[no_mangle]
pub const extern "C" fn PR_LIST_HEAD(l: *const PRCList) -> *mut PRCList {
    unsafe { (*l).next }
}
// #define PR_LIST_TAIL(_l) (_l)->prev

#[no_mangle]
pub const extern "C" fn PR_LIST_TAIL(l: *const PRCList) -> *mut PRCList {
    unsafe { (*l).prev }
}

/*
 * Remove the element "_e" from it's circular list.
 */
// #define PR_REMOVE_LINK(_e)         \
//     PR_BEGIN_MACRO             \
//     (_e)->prev->next = (_e)->next; \
//     (_e)->next->prev = (_e)->prev; \
//     PR_END_MACRO

#[no_mangle]
pub extern "C" fn PR_REMOVE_LINK(e: *mut PRCList) {
    unsafe {
        (*(*e).prev).next = (*e).next;
        (*(*e).next).prev = (*e).prev;
    }
}

/*
 * Remove the element "_e" from it's circular list. Also initializes the
 * linkage.
 */
// #define PR_REMOVE_AND_INIT_LINK(_e)    \
//     PR_BEGIN_MACRO             \
//     (_e)->prev->next = (_e)->next; \
//     (_e)->next->prev = (_e)->prev; \
//     (_e)->next = (_e);         \
//     (_e)->prev = (_e);         \
//     PR_END_MACRO
#[no_mangle]
pub extern "C" fn PR_REMOVE_AND_INIT_LINK(e: *mut PRCList) {
    unsafe {
        (*(*e).prev).next = (*e).next;
        (*(*e).next).prev = (*e).prev;
        (*e).next = e;
        (*e).prev = e;
    }
}

/*
 * Return non-zero if the given circular list "_l" is empty, zero if the
 * circular list is not empty
 */
// #define PR_CLIST_IS_EMPTY(_l) \
//     ((_l)->next == (_l))

#[no_mangle]
pub extern "C" fn PR_CLIST_IS_EMPTY(l: *const PRCList) -> PRBool {
    PRBool::from(unsafe { ptr::eq((*l).next.cast_const(), l) })
}

/*
 * Initialize a circular list
 */
// #define PR_INIT_CLIST(_l)  \
//     PR_BEGIN_MACRO     \
//     (_l)->next = (_l); \
//     (_l)->prev = (_l); \
//     PR_END_MACRO

#[no_mangle]
pub extern "C" fn PR_INIT_CLIST(l: *mut PRCList) {
    unsafe {
        (*l).next = l;
        (*l).prev = l;
    }
}

// #define PR_INIT_STATIC_CLIST(_l) \
//     {(_l), (_l)}
//
// #endif /* prclist_h___ */
