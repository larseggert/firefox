/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

fn main() {
    cfg_aliases::cfg_aliases! {
        kqueue: { any(target_os = "macos", target_os = "ios", target_os = "freebsd", target_os = "openbsd", target_os = "netbsd", target_os = "dragonfly") },
        bsd_kqueue: { any(target_os = "ios", target_os = "freebsd", target_os = "openbsd", target_os = "netbsd", target_os = "dragonfly") },
        epoll: { any(target_os = "linux", target_os = "android") },
        poll: { not(any(kqueue, epoll, windows)) },
        pipe_notify: { any(target_os = "openbsd", target_os = "netbsd") },
    }
}
