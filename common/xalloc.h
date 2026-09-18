/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

/*
 * xalloc.h
 *
 * Simple error-checking allocation functions - internal header
 *
 */

#ifndef TFTP_XALLOC_H
#define TFTP_XALLOC_H

#include "config.h"
#include "tftpsubs.h"

extern void (*out_of_memory)(void);

noreturn void xalloc_out_of_memory(void);

/* This function is used to check for memory allocation failure (only) */
static inline void *check_null(void *p)
{
    if (!p) {
        xalloc_out_of_memory();
        abort();
    }

    return p;
}

#endif /* TFTP_XALLOC_H */
