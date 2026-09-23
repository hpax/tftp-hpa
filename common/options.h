/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (C) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#ifndef COMMON_OPTIONS_H
#define COMMON_OPTIONS_H

#include "config.h"

struct common_options {
    int ai_fam;
    unsigned int portrange_from;
    unsigned int portrange_to;
    int blksize;                /* Relative to MTU if <= 0 */
    unsigned int max_windowsize;
    uintmax_t rexmtval;     /* Initial retransmission timeout (us) */
};

extern struct common_options xopt;

#endif /* COMMON_OPTIONS_H */
