/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#ifndef TFTPSUBS_H
#define TFTPSUBS_H

#include "config.h"
#include "common/tftp.h"

extern const char *_progname;
extern pid_t _progpid;
void set_progname(const char *);	/* main() should pass argv[0] here */
void post_fork(void);                   /* Invoke in child after fork() */

extern void (*out_of_memory)(void);	/* Optional out of memory handler */
MALLOC_FUNC void *xmalloc(size_t);
CALLOC_FUNC void *xcalloc(size_t, size_t);
REALLOC_FUNC void *xrealloc(void *, size_t);
NEWBUF_FUNC char *xstrdup(const char *);
static inline void xfree(void *ptr)
{
    /* This is paranoia: free() is supposed to handle NULL already */
    if (ptr)
        free(ptr);
}

/*
 * Allocates a buffer for the given pointer, for xnewn() with an array count.
 */
#define xnewn(ptr,n)                            \
    do {                                        \
        void **pp = (void **)&(ptr);            \
        void *p = xcalloc(n, sizeof *(ptr));    \
        *pp = p;                                \
    } while (0)

#define xnew(ptr) xnewn(ptr,1)

/*
 * Frees a value and sets the corresponding pointer to NULL.
 * This blindly assumes all data pointers have the same representation;
 * otherwise this would require using typeof().
 */
#define xdelete(ptr)                                    \
    do {                                                \
        void **pp = (void **)&(ptr);                    \
        void *p = *pp;                                  \
        if (p) {                                        \
            *pp = NULL;                                 \
            free(p);                                    \
        }                                               \
    } while (0)

/*
 * Equivalent, but it takes a pointer to a pointer which might be NULL
 * itself.
 */
#define xdeletep(ptrp)                                  \
    do {                                                \
        void **pp = (void **)(ptrp);                    \
        if (pp) {                                       \
            void *p = *pp;                              \
            if (p) {                                    \
                *pp = NULL;                             \
                free(p);                                \
            }                                           \
        }                                               \
    } while (0)

/*
 * Clears a buffer based on its type
 */
#define xzeron(ptr,n) memset((ptr), 0, (n)*sizeof *(ptr))
#define xzero(ptr)    xzeron(ptr,1)

/*
 * Error checking versions of [v]asprintf()
 */
PRINTF_FUNC(2,3) int xasprintf(char **strp, const char *fmt, ...);
PRINTF_FUNC(2,0) int xvasprintf(char **strp, const char *fmt, va_list ap);

#ifndef HAVE_RANDOM
static inline long random(void)
{
    return rand();
}
static inline void srandom(unsigned int seed)
{
    return srand(seed);
}
#endif

void random_init(void);
extern uint32_t (*random_u32)(void);

/* ... these might be made fancier at some point ... */
static inline void random_post_fork_parent(void)
{
    /* Advance the PRNG at least once per client */
    (void)random();
}
static inline void random_post_fork_child(void)
{
}

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
bool is_numeric_ipv6(const char *);
char *strip_address(char *);
#else
#define is_numeric_ipv6(a)      false
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
int tftp_recv_time(int, void *, int, unsigned int, struct sockaddr *,
                   socklen_t *, unsigned long *);
const char *net_family(sa_family_t);
char *net_address(const struct sockaddr *, socklen_t);

unsigned int tftp_mtu_blksize(int fd, const union sock_addr *sa, int adjust);

/*
 * Wrappers for get/setsockopt() for the case where the option is an int.
 */
int getsockint(int sockfd, int level, int optname);

static inline int setsockint(int sockfd, int level, int optname,
                             const int optval)
{
    return setsockopt(sockfd, level, optname, &optval, sizeof(optval));
}

extern int segsize;
#define MIN_SEGSIZE	8       /* Really impractically small, but... */
#define MAX_SEGSIZE	65464

int pick_port_bind(int sockfd, union sock_addr *myaddr);

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

static inline unsigned char ascii_tolower(unsigned char c)
{
    if ((unsigned char)(c - 'A') <= (unsigned char)('Z' - 'A'))
        c += 'a' - 'A';
    return c;
}

bool ascii_strcaseeq(const char *s1, const char *s2);
bool ascii_strncaseeq(const char *s1, const char *s2, size_t n);

/*
 * A conservative estimate of the maximum number of decimal digits
 * that can represent a certain unsigned integer type
 */
#define DIGIT_SPACE(x) ((sizeof(x)*5+1) >> 1)

/*
 * Packet type and error name functions, for printing messages.
 * packet_type() and error_msg() return a pointer into static memory;
 * errpkt_to_string() allocates a string in heap storage.
 *
 * If error_msg() is passed a negative value, then it is assumed to be
 * -errno, and the string is passed to strerror().
 */
const char *error_msg(int err);
char *errpkt_to_string(const struct tftphdr *tp, int n);
int make_errpacket(struct tftphdr **tpp, int error, const char *msg);

extern const char * const packet_types[PTYPE_CNT];
extern const char * const errmsgs[ETYPE_CNT];

static inline const char *packet_type(uint16_t opcode)
{
    if (opcode >= PTYPE_CNT)
        return "unknown";
    else
        return packet_types[opcode];
}

#endif
