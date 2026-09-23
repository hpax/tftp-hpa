/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#include "common/tftpsubs.h"
#include "cap.h"
#include "version.h"

#ifdef WITH_REGEX
#define WITH_REGEX_STR "yes"
#else
#define WITH_REGEX_STR "no"
#endif

#ifdef HAVE_PTHREAD
#define WITH_THREADS_STR "yes"
#else
#define WITH_THREADS_STR "no"
#endif

const char version_string[] = VERSION;

void print_configuration(FILE *f)
{
    fprintf(f, "%s\n"
            "Filename remap: %s\n"
            "I/O threads:    %s\n"
            "Capabilities:   %s\n",
            version_string,
            WITH_REGEX_STR,
            WITH_THREADS_STR,
            capset_get_type());
}
