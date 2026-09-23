/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (C) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#ifndef TFTP_OPTIONS_H
#define TFTP_OPTIONS_H

#include "common/options.h"

struct modes;

/*
 * Client configuration selected at startup or by an interactive command.
 * Connection and transfer state deliberately remain outside this structure.
 */
struct tftp_options {
    const struct modes *mode;
    uintmax_t maxtimeout;
    int verbose;
    bool trace;
    bool literal;
    bool tsize;
    bool iscmd;
    bool no_options;
    bool unsafe;
};

extern struct tftp_options copt;

#endif /* TFTP_OPTIONS_H */
