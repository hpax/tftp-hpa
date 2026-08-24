/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 * All rights reserved.
 */

/*
 * Create sockets for and listen on one or more addresses, and add
 * them to a given pollset. If ai_fam is not AI_UNSPEC, only listen on
 * that specific family.
 */

#include "config.h"             /* Must be included first */
#include "tftpd.h"
#include "common/pollset.h"

#ifndef AI_IDN
#define AI_IDN 0
#endif

#ifdef HAVE_IPV6
#define ADDRLEN INET6_ADDRSTRLEN
#else
#define ADDRLEN INET_ADDRSTRLEN
#endif

static const char *famname(int ai_fam)
{
    switch (ai_fam) {
    case AF_INET:
        return "IPv4 ";
#ifdef HAVE_IPV6
    case AF_INET6:
        return "IPv6 ";
#endif
    default:
        return "";
    }
}

static char *addrstr(const struct sockaddr *addr)
{
    char *addrbuf = xmalloc(ADDRLEN + 6);
    char *pp;
    if (!inet_ntop(addr->sa_family, SOCKADDR_P(addr), addrbuf, ADDRLEN))
        strcpy(addrbuf, "<invalid>");

    pp = strchr(addrbuf, '\0');
    sprintf(pp, ":%u", ntohs(SOCKPORT(addr)));

    return addrbuf;
}

int listen_to(struct pollset *set, const char *name, int ai_fam)
{
    char *ns = xstrdup(name);
    char *np = ns;
    const char *hostname = ns;
    const char *service  = NULL;
    struct addrinfo hints;
    struct addrinfo *addrs = NULL;
    const struct addrinfo *ai;
    int err = EINVAL;
    int nsockets = 0;

#ifndef HAVE_IPV6
    ai_fam = AF_INET;
#endif

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = ai_fam;
    hints.ai_flags  = AI_PASSIVE | AI_IDN;

    /* Allow a bracketed hostname (e.g. IPv6 address) */
    if (*np == '[') {
        hostname = ++np;
        while (*np) {
            if (*np == ']') {
                *np++ = '\0';
                break;
            }
            np++;
        }
        if (*np == ':')
            service = np+1;
    } else if (*np == '*' || *np == ':') {
        hostname = "";
        if (*np == '*')
            np++;
        if (*np == ':')
            service = np+1;
    } else {
        hostname = np;
        while (*np) {
            if (*np == ':') {
                *np++ = '\0';
                service = np;
                break;
            }
            np++;
        }
    }

    if (!*hostname)
        hostname = NULL;

    if (!service)
        service = "tftp";

    err = getaddrinfo(hostname, service, &hints, &addrs);
    if (err) {
        tftpd_log(LOG_ERR,
                  "cannot resolve local %sbind address: %s:%s (%s)",
                  famname(ai_fam), hostname, service, gai_strerror(err));
        err = ENOENT;
        goto fail;
    }

    for (ai = addrs; ai; ai = ai->ai_next) {
        int fd;

        if (ai_fam != AF_UNSPEC && ai->ai_family != ai_fam)
            continue;

        if (ai->ai_socktype != SOCK_DGRAM)
            continue;

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            err = errno;
            tftpd_log(LOG_ERR, "failed to create %ssocket: %s",
                      famname(ai->ai_family), strerror(err));
            continue;
        }

        switch (ai->ai_family) {
        case AF_INET:
#ifdef IP_FREEBIND
            setsockint(fd, IPPROTO_IP, IP_FREEBIND, 1);
#endif
            break;
#ifdef HAVE_IPV6
        case AF_INET6:
#ifdef IPV6_FREEBIND
            setsockint(fd, IPPROTO_IPV6, IPV6_FREEBIND, 1);
#endif
            break;
#endif
        default:
            break;
        }

        if (bind(fd, ai->ai_addr, ai->ai_addrlen)) {
            char *aname;
            err = errno;
            aname = addrstr(ai->ai_addr);
            tftpd_log(LOG_ERR, "failed to bind to %s: %s",
                      aname, strerror(err));
            xfree(aname);
            continue;
        }

        nsockets++;
        pollset_add(set, fd);
    }

    if (nsockets)
        err = 0;

fail:
    if (addrs)
        freeaddrinfo(addrs);
    xfree(ns);
    return err;
}
