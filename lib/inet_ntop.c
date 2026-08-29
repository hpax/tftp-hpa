/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2008 Karsten Keil
 * Copyright (c) 2026 Intel Corporation; Author: H. Peter Anvin
 */

/*
 * inet_ntop.c
 *
 * Simple version of inet_ntop()
 *
 */

#include "config.h"

extern int errno;

const char *inet_ntop(int af, const void *src,
                      char *dst, socklen_t cnt)
{
    char *p;

    switch(af) {
    case AF_INET:
        p = inet_ntoa(*((const struct in_addr *)src));
        if (p) {
            if (cnt <= strlen(p)) {
                errno = ENOSPC;
                dst = NULL;
            } else
                strcpy(dst, p);
        } else
            dst = NULL;
        break;
#ifdef HAVE_IPV6
    case AF_INET6:
        if (cnt < 40) {
            errno = ENOSPC;
            dst = NULL;
        } else {
            const struct in6_addr *a = src;
            int i;

            p = (char *)dst;
            /* we do not compress :0: to  :: */
            for (i = 0; i < 8; i++)
                p += sprintf(p, "%x:", ntohs(a->s6_addr16[i]));
            p--;
            *p = 0;
        }
        break;
#endif
    default:
        errno = EAFNOSUPPORT;
        dst = NULL;
    }
    return dst;
}
