/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#include "config.h"
#include "tftpsubs.h"

/*
 * Returns a constant string for the socket family
 */
const char *net_family(sa_family_t ai_fam)
{
    switch (ai_fam) {
    case AF_INET:
        return "IPv4 ";
#ifdef HAVE_IPV6
    case AF_INET6:
        return "IPv6 ";
#endif
#ifdef AF_UNIX
    case AF_UNIX:
        return "Unix";
#endif
#ifdef AF_UNSPEC
    case AF_UNSPEC:
        return "";
#endif
    default:
        return "unknown family";
    }
}

/*
 * Produce a string containing both the address and the port number
 * of a sockaddr.
 */
#ifdef HAVE_IPV6
#define ADDRESS_SIZE (INET6_ADDRSTRLEN+8)
#else
#define ADDRESS_SIZE (INET_ADDRSTRLEN+6)
#endif

char *net_address(const struct sockaddr *addr, socklen_t len)
{
    const union sock_addr *sa = (const union sock_addr *)addr;
    char buf[ADDRESS_SIZE];
    char *p, *q;
    sa_family_t family = sa->sa.sa_family;

    (void)len;                  /* Not needed... yet */

    switch (family) {
    case AF_INET:
        if (!inet_ntop(family, &sa->si.sin_addr, buf, sizeof buf - 6))
            return NULL;
        p = strchr(buf, '\0');
        p += snprintf(p, sizeof buf - (p - buf), ":%u",
                      ntohs(sa->si.sin_port));
        break;
#ifdef HAVE_IPV6
    case AF_INET6:
        if (!inet_ntop(family, &sa->s6.sin6_addr, buf+1, sizeof buf - 8))
            return NULL;
        p = strchr(buf, '\0');
        buf[0] = '[';
        p = strchr(buf, '\0');
        p += snprintf(p, sizeof buf - (p - buf), "]:%u",
                      ntohs(sa->s6.sin6_port));
        break;
#endif
    default:
        p = buf + snprintf(buf, sizeof buf, "[unknown family %u]", family);
        break;
    }

    *p++ = '\0';
    q = xmalloc(p - buf);
    return memcpy(q, buf, p - buf);
}
