/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

/*
 * cap.c
 *
 * Capabilities handling
 */

#include "cap.h"

#define CAP_TYPE_NONE	0
#define CAP_TYPE_LINUX	1

#ifdef HAVE_CAP_SET_PROC
#ifdef __linux__
#define CAP_TYPE CAP_TYPE_LINUX
#endif
#endif

#ifndef CAP_TYPE
#define CAP_TYPE CAP_TYPE_NONE
#endif

#if CAP_TYPE == CAP_TYPE_LINUX

#include "cap-linux.c"

#else

#include "cap-none.c"

#endif
