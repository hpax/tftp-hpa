/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2001-2026 H. Peter Anvin - All Rights Reserved */

/*
 * tftpd.h
 *
 * Prototypes for various functions that are part of the tftpd server.
 */

#ifndef TFTPD_TFTPD_H
#define TFTPD_TFTPD_H

#include "config.h"
#include <syslog.h>
#include "common/tftpsubs.h"
#include "common/pollset.h"

typedef PRINTF_FUNC(2,3) void (*log_func)(int, const char *, ...);
extern log_func tftpd_log;

void set_signal(int, void (*)(int), int);
void tftpd_sigmask(int how, const sigset_t *set, sigset_t *oset);

int listen_to(struct pollset *set, const char *name, sa_family_t ai_fam);

struct formats {
    const char *f_mode;
    const char *(*f_rewrite) (const struct formats *, const char *,
                              int, int, const char **);
    int (*f_validate) (const char *, int, const struct formats *,
                       const char **);
    void (*f_send) (const struct formats *, struct tftphdr *, int,
                    const char *);
    void (*f_recv) (const struct formats *, struct tftphdr *, int,
                    const char *, uintmax_t);
    bool f_convert;
};

#endif
