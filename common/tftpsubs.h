/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 */

/*
 * Prototypes for read-ahead/write-behind subroutines for tftp user and
 * server.
 */
#ifndef TFTPSUBS_H
#define TFTPSUBS_H

#include "config.h"

extern const char *_progname;
void set_progname(const char *); /* main() should pass argv[0] here */

extern void (*out_of_memory)(void); /* Optional out of memory handler */
void *xmalloc(size_t);
void *xcalloc(size_t, size_t);
void *xrealloc(void *, size_t);
char *xstrdup(const char *);
void xfree(void *);

union sock_addr {
    struct sockaddr     sa;
    struct sockaddr_in  si;
#ifdef HAVE_IPV6
    struct sockaddr_in6 s6;
#endif
};

#define SOCKLEN(sock) \
    (((union sock_addr*)sock)->sa.sa_family == AF_INET ? \
    (sizeof(struct sockaddr_in)) : \
    (sizeof(union sock_addr)))

#ifdef HAVE_IPV6
#define SOCKPORT(sock) \
    (((union sock_addr*)sock)->sa.sa_family == AF_INET ? \
    ((union sock_addr*)sock)->si.sin_port : \
    ((union sock_addr*)sock)->s6.sin6_port)
#else
#define SOCKPORT(sock) \
    (((union sock_addr*)sock)->si.sin_port)
#endif

#ifdef HAVE_IPV6
#define SOCKADDR_P(sock) \
    (((union sock_addr*)sock)->sa.sa_family == AF_INET ? \
    (void *)&((union sock_addr*)sock)->si.sin_addr : \
    (void *)&((union sock_addr*)sock)->s6.sin6_addr)

#else
#define SOCKADDR_P(sock) \
    ((void *)&((union sock_addr*)sock)->si.sin_addr)
#endif

#ifdef HAVE_IPV6
int is_numeric_ipv6(const char *);
char *strip_address(char *);
#else
#define is_numeric_ipv6(a)      0
#define strip_address(a)	(a)
#endif

static inline int sa_set_port(union sock_addr *s, uint16_t port)
{
       switch (s->sa.sa_family) {
       case AF_INET:
               s->si.sin_port = port;
               break;
#ifdef HAVE_IPV6
       case AF_INET6:
               s->s6.sin6_port = port;
               break;
#endif
       default:
               return -1;
       }
       return 0;
}

int set_sock_addr(char *, union sock_addr *, char **, bool);
void tftp_set_socket_buffers(int, unsigned int, unsigned int, bool);

/*
 * Wrapper for setsockopt() for the case where the option is an int.
 */
static inline int setsockint(int sockfd, int level, int optname,
                             const int optval)
{
    return setsockopt(sockfd, level, optname, &optval, sizeof(optval));
}

struct tftphdr;

struct tftphdr *r_init(void);
void read_ahead(FILE *, int);
int readit(FILE *, struct tftphdr **, int);

int synchnet(int);

struct tftphdr *w_init(void);
int write_behind(FILE *, int);
int writeit(FILE *, struct tftphdr **, int, int);

extern int segsize;
#define MAX_SEGSIZE	65464

int pick_port_bind(int sockfd, union sock_addr *myaddr,
                   unsigned int from, unsigned int to);

int get_nullfd(void);

static inline int tftp_sigmask(int how, const sigset_t *set, sigset_t *oset)
{
#ifdef HAVE_PTHREAD_SIGMASK
    return pthread_sigmask(how, set, oset);
#else
    int rv;
    do {
        rv = sigprocmask(how, set, oset) ? errno : 0;
    } while (rv == EINTR);
    return rv;
#endif
}

#endif
