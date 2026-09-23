/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

/*
 * cap.h
 *
 * Capabilities handling hooks
 */

#ifndef TFTPD_CAP_H
#define TFTPD_CAP_H 1

#include "config.h"

void capset_none(void);
void capset_before_initgroups(void);
void capset_after_initgroups(void);
void capset_before_listen(void);
void capset_before_socket_bind(void);
void capset_before_chroot(void);
void capset_before_setid(void);
void capset_drop_all(void);
const char *capset_get_type(void);

#endif /* TFTPD_CAP_H */
