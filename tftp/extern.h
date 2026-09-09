/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 */

#ifndef EXTERN_H
#define EXTERN_H

#include "config.h"

int tftp_recvfile(int, const char *, const char *, unsigned int);
int tftp_sendfile(int, const char *, const char *, unsigned int);
extern sigjmp_buf toplevel;

#endif
