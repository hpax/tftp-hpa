/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#include "common/tftpsubs.h"
#include "version.h"

#ifdef WITH_READLINE
#define WITH_READLINE_STR "yes"
#else
#define WITH_READLINE_STR "no"
#endif

const char version_string[] = VERSION;

void print_configuration(FILE *f)
{
    fprintf(f, "%s\n"
            "Readline:       %s\n",
            version_string,
            WITH_READLINE_STR);
}
