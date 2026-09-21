/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2024 Intel Corporation; Author: H. Peter Anvin
 */

/*
 * signal.c
 *
 * User-friendly wrapper around sigaction().
 */

#include "config.h"
#include "tftpsubs.h"

int tftp_signal(int signum, sighandler_t handler, int flags)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;

    return sigaction(signum, &sa, NULL);
}
