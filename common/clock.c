/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (C) 2026 H. Peter Anvin
 */

#include "config.h"
#include "clock.h"

/*
 * Get the current time as an integer number of microseconds. If
 * clock_gettime() is available, get a monotone clock instead of
 * real time clock, to avoid strange effects around leap seconds
 * or clock adjustments.
 */
#ifdef HAVE_CLOCK_GETTIME

uintmax_t clock_us(void)
{
    /* Clocks in order of preference */
    static const clockid_t clocks[] = {
#ifdef CLOCK_MONOTONIC
        CLOCK_MONOTONIC,
#endif
#ifdef CLOCK_BOOTTIME
        CLOCK_BOOTTIME,
#endif
#ifdef CLOCK_MONOTONIC_RAW
        CLOCK_MONOTONIC_RAW,
#endif
#ifdef CLOCK_TAI
        CLOCK_TAI,
#endif
#ifdef CLOCK_REALTIME
        CLOCK_REALTIME,
#endif
#ifdef CLOCK_MONOTONIC_COARSE
        CLOCK_MONOTONIC_COARSE,
#endif
#ifdef CLOCK_BOOTTIME_COARSE
        CLOCK_BOOTTIME_COARSE,
#endif
#ifdef CLOCK_TAI_COARSE
        CLOCK_TAI_COARSE,
#endif
#ifdef CLOCK_REALTIME_COARSE
        CLOCK_REALTIME_COARSE,
#endif
    };
    static const clockid_t * volatile clock_id = clocks;
    struct timespec ts;

    const clockid_t *clk_p = clock_id;

    if (clock_gettime(*clk_p, &ts)) {
        /*
         * If the pre-selected clock doesn't work, try all the clocks
         * from the beginning (in order to cope with a transient failure.)
         */
        for (clk_p = clocks; clk_p < ARRAY_END(clocks); clk_p++) {
            if (!clock_gettime(*clk_p, &ts)) {
                clock_id = clk_p; /* Remember which one worked */
                goto ok;
            }
        }

        /* No working clocks!!! */
        return 0;               /* Failure */
    }

ok:
    return timespec_to_us(&ts);
}

#else

uintmax_t clock_us(void)
{
    struct timeval tv;

    if (!gettimeofday(&tv, NULL))
        return timeval_to_us(&tv);

    return 0;                   /* Failure */
}

#endif
