/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_Poll_h
#define mozilla_net_Poll_h

#include "prio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Poll - A function with identical API to PR_Poll
 *
 * This function provides the same interface as PR_Poll but uses
 * the Rust polling crate (from netwerk/socket/poll) underneath
 * instead of NSPR's implementation.
 *
 * @param pds     Array of PRPollDesc structures describing the file
 *                descriptors to poll and their requested events
 * @param npds    Number of elements in the pds array
 * @param timeout Timeout value in PR interval units:
 *                - PR_INTERVAL_NO_WAIT (0) for non-blocking
 *                - PR_INTERVAL_NO_TIMEOUT for infinite wait
 *                - Other values specify timeout in PR intervals
 *
 * @return Number of ready descriptors on success (>= 0)
 *         0 on timeout
 *         -1 on error
 *
 * The function behavior is identical to PR_Poll:
 * - in_flags specifies which events to poll for (PR_POLL_READ, PR_POLL_WRITE, PR_POLL_EXCEPT)
 * - out_flags returns which events occurred (including PR_POLL_ERR, PR_POLL_HUP, PR_POLL_NVAL)
 */
PRInt32 Pollx(PRPollDesc* pds, PRIntn npds, PRIntervalTime timeout);

#ifdef __cplusplus
}
#endif

#endif  // mozilla_net_Poll_h
