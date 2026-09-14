/* ----------------------------------------------------------------------- *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright (c) 2026 H. Peter Anvin - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

/*
 * Parsed tftpd command-line configuration.
 */

#ifndef TFTPD_OPTIONS_H
#define TFTPD_OPTIONS_H

#include "config.h"
#include "common/options.h"
#include "strlist.h"

#define DAEMON_DEFAULT_MAP_STEPS 4096 /* Timeout after this many steps */

struct daemon_options {
    bool cancreate;
    bool secure;
    bool unixperms;
    bool standalone;
    bool nodaemon;
    bool systemd;
    bool rooted;
    bool reject_all_options;
    bool spec_umask;
    bool use_stderr;
    uintmax_t max_windowbytes;
    unsigned long rexmtval;
    int verbosity;
    int map_steps;
    intmax_t waittime;
    mode_t my_umask;
    const char *service;
    const char *user;
    const char *path_prefix;
    const char *rewrite_file;
    const char *map_test_file;
    const char *pidfile;
    struct strlist listen_addrs;
    int ndirs;
    const char * const **dirs;
};

extern struct daemon_options dopt;

#endif
