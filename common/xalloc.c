/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

/*
 * xalloc.c
 *
 * Simple error-checking memory allocation functions
 *
 */

#include "xalloc.h"

#include "config.h"
#include "tftpsubs.h"

void (*out_of_memory)(void) = NULL;

noreturn void xalloc_out_of_memory(void)
{
    if (out_of_memory)
        out_of_memory();

    /* If out_of_memory() returns and/or is not set */
    perror(_progname);
    while (1)
        exit(EX_OSERR);
}

void *xmalloc(size_t size)
{
    /* Use calloc() in the interest of safety */
    return xcalloc(1,size);
}

void *xcalloc(size_t n, size_t size)
{
    if (!n || !size)
        n = size = 1;           /* Avoid undefined behavior 0-byte allocation */
    return check_null(calloc(n,size));
}

void *xrealloc(void *p, size_t newsize)
{
    /* This is paranoia: realloc() is supposed to handle NULL */
    if (!p)
        return xmalloc(newsize);

    if (newsize == 0)
        newsize = 1;           /* Avoid undefined behavior 0-byte allocation */

    return check_null(realloc(p, newsize));
}

char *xstrdup(const char *s)
{
    return check_null(strdup(s));
}

void *xmemdup(const void *p, size_t n)
{
    char *q = xmalloc(n);
    return memcpy(q, p, n);
}
