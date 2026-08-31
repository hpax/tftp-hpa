/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 Intel Corporation; Author: H. Peter Anvin */

#ifndef TFTPD_PATH_H
#define TFTPD_PATH_H 1

#include "config.h"

const char **parse_path(const char *path, bool strict);
char *build_path(const char * const *dirs);
unsigned int compare_paths(const char * const *a, const char * const *b);

#endif /* TFTPD_PATH_H */
