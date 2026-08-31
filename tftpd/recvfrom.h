/* ----------------------------------------------------------------------- *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright 2001-2025 H. Peter Anvin - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

/*
 * recvfrom.h
 *
 * Header for recvfrom substitute and socket configuration functions
 */

#include "config.h"

int
myrecvfrom(int s, void *buf, int len, unsigned int flags,
           union sock_addr *from, union sock_addr *myaddr);
void tftpd_config_socket(int fd, bool peer);
void set_socket_nonblock(int fd, bool flag);

/* On Cygwin, a nonblocking socket can cause immediate return from select?! */
#ifdef __CYGWIN__
#define cygwin_set_socket_nonblock(fd,flag) set_socket_nonblock(fd,flag)
#else
#define cygwin_set_socket_nonblock(fd,flag) ((void)0)
#endif
