/* ----------------------------------------------------------------------- *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright 2001-2025 H. Peter Anvin - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

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

typedef void (*log_func)(int, const char *, ...);
extern log_func tftpd_log;

void set_signal(int, void (*)(int), int);

extern const char *default_service;
int listen_to(struct pollset *set, const char *name, int ai_fam);

extern int verbosity;

struct formats {
    const char *f_mode;
    char *(*f_rewrite) (const struct formats *, char *, int, int, const char **);
    int (*f_validate) (char *, int, const struct formats *, const char **);
    void (*f_send) (const struct formats *, struct tftphdr *, int);
    void (*f_recv) (const struct formats *, struct tftphdr *, int);
    bool f_convert;
};

#endif
