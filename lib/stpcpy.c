/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 */

/*
 * stpcpy()
 */

#include "config.h"

char *stpcpy(char *dst, const char *src)
{
    size_t len = strlen(src);
    memcpy(dst, src, len+1);
    return dst+len;
}
