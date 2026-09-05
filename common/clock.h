/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (C) 2026 H. Peter Anvin
 */

#ifndef TFTP_CLOCK_H
#define TFTP_CLOCK_H 1

#include "config.h"

#define USEC_PER_SEC	1000000UL
#define NSEC_PER_SEC	1000000000UL
#define NSEC_PER_USEC	1000UL
#define USEC_PER_MSEC	1000UL

#ifdef HAVE_STRUCT_TIMESPEC

static inline uintmax_t timespec_to_us(const struct timespec *ts)
{
    return ((uintmax_t)ts->tv_sec * USEC_PER_SEC) +
        (ts->tv_nsec / NSEC_PER_USEC);
}

static inline struct timespec *
us_to_timespec(uintmax_t us, struct timespec *ts)
{
    ts->tv_sec  = us / USEC_PER_SEC;
    ts->tv_nsec = (us % USEC_PER_SEC) * NSEC_PER_USEC;
    return ts;
}

#endif

static inline uintmax_t timeval_to_us(const struct timeval *tv)
{
    return ((uintmax_t)tv->tv_sec * USEC_PER_SEC) + tv->tv_usec;
}

static inline struct timeval *
us_to_timeval(uintmax_t us, struct timeval *tv)
{
    tv->tv_sec  = us / USEC_PER_SEC;
    tv->tv_usec = us % USEC_PER_SEC;
    return tv;
}

uintmax_t clock_us(void);

#endif /* TFTP_CLOCK_H */
