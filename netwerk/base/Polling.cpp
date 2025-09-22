/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "prio.h"
#include "private/primpl.h"
#include "private/pprio.h"
#include "nsTArray.h"
#include "mozilla/net/poll.h"
#include <stdint.h>
#include <stdio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <errno.h>

// Debug helper to check socket state and pending data
static void DebugFdState(int fd, const char* label) {
  int bytes_available = 0;
  errno = 0;
  int err = ioctl(fd, FIONREAD, &bytes_available);
  int ioctl_errno = errno;

  // Also do a quick poll(0) to check current state
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN | POLLOUT | POLLPRI | POLLERR | POLLHUP;
  pfd.revents = 0;
  int poll_result = poll(&pfd, 1, 0);

  // Try to get socket type
  int sock_type = 0;
  socklen_t optlen = sizeof(sock_type);
  int getsockopt_err = getsockopt(fd, SOL_SOCKET, SO_TYPE, &sock_type, &optlen);

  fprintf(stderr, "  DEBUG fd=%d (%s): ioctl_err=%d errno=%d bytes=%d poll=%d revents=0x%x socktype=%d",
          fd, label, err, ioctl_errno, bytes_available, poll_result, pfd.revents,
          getsockopt_err == 0 ? sock_type : -1);
  if (pfd.revents & POLLIN) fprintf(stderr, " POLLIN");
  if (pfd.revents & POLLOUT) fprintf(stderr, " POLLOUT");
  if (pfd.revents & POLLPRI) fprintf(stderr, " POLLPRI");
  if (pfd.revents & POLLERR) fprintf(stderr, " POLLERR");
  if (pfd.revents & POLLHUP) fprintf(stderr, " POLLHUP");
  if (pfd.revents & POLLNVAL) fprintf(stderr, " POLLNVAL");
  fprintf(stderr, "\n");
  fflush(stderr);
}

// Local internal flags for tracking the mapping between NSPR poll requests
// and system poll events. These use higher bits than PR_POLL_* (0x01-0x3F)
// to avoid collision, allowing us to safely clear them without affecting
// the result flags.
#define POLLX_READ_SYS_READ 0x0100
#define POLLX_READ_SYS_WRITE 0x0200
#define POLLX_WRITE_SYS_READ 0x0400
#define POLLX_WRITE_SYS_WRITE 0x0800
#define POLLX_INTERNAL_FLAGS_MASK 0x0F00

static PRBool pt_TestAbort(void) {
  PRThread* me = PR_GetCurrentThread();
  if (_PT_THREAD_INTERRUPTED(me)) {
    PR_SetError(PR_PENDING_INTERRUPT_ERROR, 0);
    me->state &= ~PT_THREAD_ABORTED;
    return PR_TRUE;
  }
  return PR_FALSE;
} /* pt_TestAbort */

// Check if we should use PR_Poll instead of Pollx (for debugging comparison)
static bool UsePRPoll() {
  static int use_pr_poll = -1;
  if (use_pr_poll == -1) {
    use_pr_poll = (getenv("MOZ_USE_PR_POLL") != nullptr) ? 1 : 0;
    if (use_pr_poll) {
      fprintf(stderr, "DEBUG: Using PR_Poll instead of Pollx\n");
    }
  }
  return use_pr_poll == 1;
}

// Log the poll call inputs and outputs for debugging
static void LogPollCall(const char* impl, PRPollDesc* pds, PRIntn npds,
                        PRIntervalTime timeout, PRInt32 result) {
  PRIntn msecs;
  switch (timeout) {
    case PR_INTERVAL_NO_WAIT: msecs = 0; break;
    case PR_INTERVAL_NO_TIMEOUT: msecs = -1; break;
    default: msecs = PR_IntervalToMilliseconds(timeout); break;
  }
  fprintf(stderr, "%s: npds=%d msecs=%d result=%d out_flags=[", impl, npds, msecs, result);
  for (PRIntn j = 0; j < npds; ++j) {
    fprintf(stderr, "%s0x%x", j > 0 ? "," : "", pds[j].out_flags);
  }
  fprintf(stderr, "]\n");
  fflush(stderr);
}

// Poll - A function with identical API to PR_Poll that uses
// the Rust polling crate implementation underneath
extern "C" PRInt32 Pollx(PRPollDesc* pds, PRIntn npds, PRIntervalTime timeout) {
  // Allow switching to PR_Poll via environment variable for debugging
  if (UsePRPoll()) {
    PRInt32 result = PR_Poll(pds, npds, timeout);
    LogPollCall("PR_Poll", pds, npds, timeout, result);
    return result;
  }

  PRInt32 ready = 0;
  /*
   * For restarting poll() if it is interrupted by a signal.
   * We use these variables to figure out how much time has
   * elapsed and how much of the timeout still remains.
   */
  PRIntervalTime start = 0, elapsed, remaining;

  // FIXME: This is a private static function in ptio.c - do we need it?
  if (pt_TestAbort()) {
    return -1;
  }

  if (0 == npds) {
    PR_Sleep(timeout);
  } else {
    PRIntn index, msecs;

    Poller* poller = poll_new();
    MOZ_RELEASE_ASSERT(poller);

    for (index = 0; index < npds; ++index) {
      PRInt16 in_flags_read = 0, in_flags_write = 0;
      PRInt16 out_flags_read = 0, out_flags_write = 0;

      if ((NULL != pds[index].fd) && (0 != pds[index].in_flags)) {
        if (pds[index].in_flags & PR_POLL_READ) {
          in_flags_read = (pds[index].fd->methods->poll)(
              pds[index].fd, pds[index].in_flags & ~PR_POLL_WRITE,
              &out_flags_read);
          fprintf(stderr, "Pollx: index=%d NSPR poll(READ) -> in_flags_read=0x%x out_flags_read=0x%x\n",
                  index, in_flags_read, out_flags_read);
        }
        if (pds[index].in_flags & PR_POLL_WRITE) {
          in_flags_write = (pds[index].fd->methods->poll)(
              pds[index].fd, pds[index].in_flags & ~PR_POLL_READ,
              &out_flags_write);
          fprintf(stderr, "Pollx: index=%d NSPR poll(WRITE) -> in_flags_write=0x%x out_flags_write=0x%x\n",
                  index, in_flags_write, out_flags_write);
        }
        if ((0 != (in_flags_read & out_flags_read)) ||
            (0 != (in_flags_write & out_flags_write))) {
          /* this one is ready right now */
          fprintf(stderr, "Pollx: index=%d READY NOW (layer has data): in_read=0x%x out_read=0x%x in_write=0x%x out_write=0x%x\n",
                  index, in_flags_read, out_flags_read, in_flags_write, out_flags_write);
          if (0 == ready) {
            /*
             * We will return without calling the system
             * poll function.  So zero the out_flags
             * fields of all the poll descriptors before
             * this one.
             */
            for (int i = 0; i < index; i++) {
              pds[i].out_flags = 0;
            }
          }
          ready += 1;
          pds[index].out_flags = out_flags_read | out_flags_write;
        } else {
          /* now locate the NSPR layer at the bottom of the stack */
          PRFileDesc* bottom =
              PR_GetIdentitiesLayer(pds[index].fd, PR_NSPR_IO_LAYER);
          /* ignore a socket without PR_NSPR_IO_LAYER available */

          pds[index].out_flags = 0; /* pre-condition */
          if ((NULL != bottom) &&
              ((PRInt32)_PR_FILEDESC_OPEN == bottom->secret->state)) {
            if (0 == ready) {
              // syspoll[index].fd = bottom->secret->md.osfd;
              // syspoll[index].events = 0;
              bool readable = false;
              bool writable = false;
              bool priority = false;
              if (in_flags_read & PR_POLL_READ) {
                pds[index].out_flags |= POLLX_READ_SYS_READ;
                // syspoll[index].events |= POLLIN;
                readable = true;
              }
              if (in_flags_read & PR_POLL_WRITE) {
                pds[index].out_flags |= POLLX_READ_SYS_WRITE;
                // syspoll[index].events |= POLLOUT;
                writable = true;
              }
              if (in_flags_write & PR_POLL_READ) {
                pds[index].out_flags |= POLLX_WRITE_SYS_READ;
                // syspoll[index].events |= POLLIN;
                readable = true;
              }
              if (in_flags_write & PR_POLL_WRITE) {
                pds[index].out_flags |= POLLX_WRITE_SYS_WRITE;
                // syspoll[index].events |= POLLOUT;
                writable = true;
              }
              if (pds[index].in_flags & PR_POLL_EXCEPT) {
                // syspoll[index].events |= POLLPRI;
                priority = true;
              }

              MOZ_RELEASE_ASSERT(readable || writable || priority);

              // Get the OS file descriptor
              // PROsfd osfd = PR_FileDesc2NativeHandle(bottom);
              PROsfd osfd = bottom->secret->md.osfd;
              MOZ_RELEASE_ASSERT(osfd >= 0);

              // Create a poll event and add it
              PollEvent event = poll_event_new(index, readable, writable);
              event.priority = priority;

              PollResult result = poll_add(poller, osfd, event);
              if (result != PollResult::Ok) {
                fprintf(stderr,
                        "Pollx: poll_add failed for fd=%d readable=%d "
                        "writable=%d priority=%d\n",
                        osfd, readable, writable, priority);
              }
              MOZ_RELEASE_ASSERT(result == PollResult::Ok);
            }
          } else {
            if (0 == ready) {
              for (int i = 0; i < index; i++) {
                pds[i].out_flags = 0;
              }
            }
            ready += 1; /* this will cause an abrupt return */
            pds[index].out_flags = PR_POLL_NVAL; /* bogii */
          }
        }
      } else {
        /* make poll() ignore this entry */
        // syspoll[index].fd = -1;
        // syspoll[index].events = 0;
        pds[index].out_flags = 0;
      }
    }
    if (0 == ready) {
      switch (timeout) {
        case PR_INTERVAL_NO_WAIT:
          msecs = 0;
          break;
        case PR_INTERVAL_NO_TIMEOUT:
          msecs = -1;
          break;
        default:
          msecs = PR_IntervalToMilliseconds(timeout);
          start = PR_IntervalNow();
      }

      fprintf(stderr, "Pollx: about to wait, poll_len=%zu npds=%d msecs=%d\n",
              poll_len(poller), npds, msecs);
      fflush(stderr);

      // Debug: Check state of all fds before waiting
      fprintf(stderr, "Pollx: checking fd states before wait:\n");
      for (PRIntn j = 0; j < npds; ++j) {
        if (pds[j].fd != NULL) {
          PRFileDesc* bottom = PR_GetIdentitiesLayer(pds[j].fd, PR_NSPR_IO_LAYER);
          if (bottom) {
            PROsfd osfd = bottom->secret->md.osfd;
            char label[64];
            snprintf(label, sizeof(label), "index=%d in_flags=0x%x", j, pds[j].in_flags);
            DebugFdState(osfd, label);
          }
        }
      }
      fflush(stderr);

    retry:
      nsTArray<PollEvent> events;
      ready = poll_wait(poller, &events, msecs);
      fprintf(stderr, "Pollx: poll_wait returned ready=%d events=%zu msecs=%d\n",
              ready, events.Length(), msecs);
      fflush(stderr);
      // ready = poll(syspoll, npds, msecs);
      if (-1 == ready) {
        PRIntn oserror = errno;

        if (EINTR == oserror) {
          if (timeout == PR_INTERVAL_NO_TIMEOUT) {
            goto retry;
          } else if (timeout == PR_INTERVAL_NO_WAIT) {
            ready = 0; /* don't retry, just time out */
          } else {
            elapsed = (PRIntervalTime)(PR_IntervalNow() - start);
            if (elapsed > timeout) {
              ready = 0; /* timed out */
            } else {
              remaining = timeout - elapsed;
              msecs = PR_IntervalToMilliseconds(remaining);
              goto retry;
            }
          }
        } else {
          // FIXME: Not available at least on Darwin.
          // _PR_MD_MAP_POLL_ERROR(oserror);
          fprintf(stderr, "_PR_MD_MAP_POLL_ERROR(oserror)");
        }
      } else if (ready > 0) {
        for (size_t i = 0; i < events.Length(); i++) {
          const PollEvent& event = events[i];
          fprintf(stderr,
                  "Pollx: event[%zu] key=%zu readable=%d writable=%d "
                  "priority=%d error=%d hup=%d\n",
                  i, event.key, event.readable, event.writable, event.priority,
                  event.error, event.hup);
          const auto index = event.key;
          PRInt16 out_flags = 0;
          PRInt16 prev_out_flags = pds[index].out_flags;
          if ((NULL != pds[index].fd) && (0 != pds[index].in_flags)) {
            if (event.readable || event.writable || event.priority ||
                event.error || event.hup /*0 != syspoll[index].revents*/) {
              if (event.readable /*syspoll[index].revents & POLLIN*/) {
                if (pds[index].out_flags & POLLX_READ_SYS_READ) {
                  out_flags |= PR_POLL_READ;
                }
                if (pds[index].out_flags & POLLX_WRITE_SYS_READ) {
                  out_flags |= PR_POLL_WRITE;
                }
              }
              if (event.writable /*syspoll[index].revents & POLLOUT*/) {
                if (pds[index].out_flags & POLLX_READ_SYS_WRITE) {
                  out_flags |= PR_POLL_READ;
                }
                if (pds[index].out_flags & POLLX_WRITE_SYS_WRITE) {
                  out_flags |= PR_POLL_WRITE;
                }
              }
              if (event.priority /*syspoll[index].revents & POLLPRI*/) {
                out_flags |= PR_POLL_EXCEPT;
              }
              if (event.error /*syspoll[index].revents & POLLERR*/) {
                out_flags |= PR_POLL_ERR;
              }
              if (event.hup /*syspoll[index].revents & POLLHUP*/) {
                out_flags |= PR_POLL_HUP;
              }
            }
          }
          // We get separate events from poll_wait for descriptors that are,
          // e.g., both readable and writable so we need to accumulate the flags
          // into out_flags.
          pds[index].out_flags |= out_flags;
          fprintf(stderr,
                  "Pollx: index=%zu prev_out_flags=0x%x computed_out_flags=0x%x final_out_flags=0x%x\n",
                  index, prev_out_flags, out_flags, pds[index].out_flags);
        }
      }

      // Clear internal flags from out_flags for all descriptors.
      // Since our POLLX_* internal flags use bits 0x0100-0x0800 (different
      // from PR_POLL_* result flags which use 0x01-0x3F), this clearing
      // won't affect the actual result flags.
      for (PRIntn j = 0; j < npds; ++j) {
        pds[j].out_flags &= ~POLLX_INTERNAL_FLAGS_MASK;
      }

      LogPollCall("Pollx", pds, npds, timeout, ready);
    }
    poll_free(poller);
  }
  return ready;
}
