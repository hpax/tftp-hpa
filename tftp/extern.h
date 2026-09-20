/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1993
 *	The Regents of the University of California.
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 *  All rights reserved.
 */

#ifndef TFTP_EXTERN_H
#define TFTP_EXTERN_H

#include "config.h"
#include "common/tftpsubs.h"

struct server_info {
    union sock_addr addr;
    socklen_t addrlen;
    bool connected;
    char *host;
    char *canonname;
    char *addr_str;
};

extern struct server_info serv;
extern sigjmp_buf toplevel;

int tftp_recvfile(int, const char *, const char *);
int tftp_sendfile(int, const char *, const char *);

#endif
