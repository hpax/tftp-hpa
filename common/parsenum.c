/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (C) 2026 H. Peter Anvin <hpa@zytor.com>
 *
 * Parse a decimal number, with validation and support for optional
 * unit suffixes.
 *
 * Returns BAD_NUM = uintmax_t maximum on any errors.
 */

#include "config.h"
#include "tftpsubs.h"
#include "common/options.h"

#include <ctype.h>

uintmax_t parse_uint(const char *arg, uintmax_t lo, uintmax_t hi)
{
    char *end;
    uintmax_t parsed, shifted;
    unsigned int shift;

    if (!arg)
        return BAD_NUM;

    errno = 0;
    parsed = strtoumax(arg, &end, 10);
    if (errno || end == arg)
        return BAD_NUM;

    while (isspace((unsigned char)*end))
        end++;

    switch (ascii_tolower(*end)) {
    case '\0':
        shift = 0;
        break;
    case 'k':
        shift = 10;
        break;
    case 'm':
        shift = 20;
        break;
    case 'g':
        shift = 30;
        break;
    case 't':
        shift = 40;
        break;
    case 'p':
        shift = 50;
        break;
    case 'e':
        shift = 60;
        break;
    default:
        return BAD_NUM;
    }

    shifted = parsed;
    if (shift) {
       shifted = parsed << shift;
        if ((shifted >> shift) != parsed)
            return BAD_NUM;     /* Overflow! */

        end++;
        if (*end)
            return BAD_NUM;     /* Garbage after value */
    }

    if (shifted < lo || shifted > hi)
        return BAD_NUM;

    return shifted;
}

bool parse_blocksize_arg(const char *str, unsigned int minimum)
{
    char *vp;
    bool ok = false;
    uintmax_t v;
    int blksize = 0;

    if (ascii_strncaseeq(str, "mtu", 3)) {
        switch (str[3]) {
        case '\0':
            xopt.blksize = 0;
            return true;
        case '-':
            v = parse_uint(str+4, 0, INT_MAX/2);
            if (v == BAD_NUM)
                return false;
            xopt.blksize = -v;
            return true;
        default:
            return false;
        }
    } else {
        v = parse_uint(str, minimum, MAX_SEGSIZE);
        if (v == BAD_NUM)
            return false;
        xopt.blksize = v;
        return true;
    }
}
