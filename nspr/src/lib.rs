// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#![expect(non_camel_case_types, non_snake_case, unused, reason = "NSPR code")]

mod plarena;
mod plbase64;
mod plstr;
mod pprio;
mod prclist;
mod prcvar;
mod prdtoa;
mod prenv;
mod prerr;
mod prerror;
mod prfile;
mod prinit;
mod prinrval;
mod prio;
mod prlink;
mod prlock;
mod prlog;
mod prmem;
mod prmon;
mod prnetdb;
mod prrwlock;
mod prshma;
mod prsystem;
mod prthread;
mod prtime;
mod prtpool;
mod prtypes;

pub const PR_BITS_PER_BYTE: i32 = 8;
pub const PR_BYTES_PER_INT: i32 = 4;

// FIXME: This doesn't work.
// pub const IS_LITTLE_ENDIAN: bool = cfg!(target_endian = "little");
// pub const IS_BIG_ENDIAN: bool = cfg!(target_endian = "big");
// pub const HAVE_LONG_LONG: bool = cfg!(target_pointer_width = "64");

pub const IS_LITTLE_ENDIAN: bool = true;
pub const IS_BIG_ENDIAN: bool = false;
pub const HAVE_LONG_LONG: bool = true;
