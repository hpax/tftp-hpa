/* ----------------------------------------------------------------------- *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright 2001-2007 H. Peter Anvin - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

/*
 * misc.c
 *
 * Minor help routines.
 */

#include "tftpd.h"

/*
 * Set the signal handler and flags, and error out on failure.
 */
void set_signal(int signum, sighandler_t handler, int flags)
{
    if (tftp_signal(signum, handler, flags)) {
        tftpd_log(LOG_ERR, "sigaction: %s", strerror(errno));
        exit(EX_OSERR);
    }
}
