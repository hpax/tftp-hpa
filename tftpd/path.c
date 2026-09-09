/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Intel Corporation; Author: H. Peter Anvin
 */

/*
 * This breaks down a path name into its constituents, removing
 * . and .. components.
 *
 * Returns NULL if the path is invalid (notably if it requires access
 * above the topmost directory level.)
 *
 * To match historical behavior, the path must be absolute, starting
 * with an explicit /.
 *
 * This returns a list of pointers into a string array, all allocated
 * as a single heap allocation.
 */

#include "config.h"
#include "common/tftpsubs.h"
#include "path.h"

/*
 * Validate a filename character. On Unix, disallow ASCII control
 * characters (< 32, 127); on Windows, also disallow \ : and other
 * characters disallowed in file names (use rewrite to translate \
 * into / just as on Unix, if desired.)
 */
static bool is_valid_char(unsigned char c)
{
    switch (c) {
#ifdef _WIN32
    case '<':
    case '>':
    case ':':
    case '"':
    case '\\':                  /* Slashes only for pathname separators! */
    case '|':
    case '?':
    case '*':
        return false;
#endif
    default:
        return c >= ' ';
    }
}

/*
 * Return true if the given character is a pathname separator that
 * should be collapsed when merging paths.
 */
static bool is_dir_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/*
 * Validate a filename (path component) for the platform, beyond what
 * is_valid_char() does.
 */
#ifndef _WIN32
#define is_valid_filename(file) true
#else
/*
 * This is Win32-specific code; assume _strnicmp() exists.
 *
 * Illegal on Windows: names ending in ' ', '.'
 * One of 22 special names either standalone or followed by '.' + anything,
 * case insensitive.
 */
static bool is_valid_filename(const char *file)
{
    const char *p;
    bool last_ok = false;
    char c;

    p = file;
    do {
        c = *p;
        switch (c) {
        case '.':
            last_ok = false;
            /* fall through */
        case '\0':
            switch (p-file) {
            case 3:
                if (!_strnicmp(file, "aux", 3) ||
                    !_strnicmp(file, "con", 3) ||
                    !_strnicmp(file, "nul", 3) ||
                    !_strnicmp(file, "prn", 3))
                    return false;
                break;
            case 4:
                if ((unsigned char)(p[-1] - '1') < 9) {
                    if (!_strnicmp(file, "com", 3) ||
                        !_strnicmp(file, "lpt", 3))
                        return false;
                }
                break;
            default:
                break;
            }
            break;
        case ' ':
            last_ok = false;
            break;
        default:
            last_ok = true;
            break;
        }
        p++;
    } while (c);

    return last_ok;
}
#endif /* _WIN32 */

/*
 * Canonicalize a path by removing components (. and ..) and validate illegal
 * filenames (currently only on Windows) beyond what is_valid_char()
 * can do.
 *
 * If successful, return the number of entries dropped (in which case *dpp
 * will have been adjusted accordingly), or return -1 if the filename
 * is invalid or would ascend past the root.
 */
static int adjust_tail(const char ***dpp, const char **top)
{
    size_t drop = 0;
    const char **dp = *dpp;
    const char *last;

    /* These are sanity checks which should never trigger, but... */
    if (!dp || dp < top)
        return -1;
    else if (dp == top)
        return 0;

    /* Get the last path component */
    last = dp[-1];

    if (!is_valid_filename(last))
        return -1;

    if (last[0] == '.') {
        if (!last[1]) {
            /* . component */
            drop = 1;
        } else if (last[1] == '.' && !last[2]) {
            /* .. component */
            drop = 2;
        }

        if (dp < top+drop)
            return -1;

        *dpp -= drop;
        return (int)drop;
    } else {
        return 0;
    }
}

/*
 * Parse, validate and canonicalize a path. If "strict" is set,
 * reject paths that contain . or .. components as well as paths
 * without a leading slash.
 */
const char **parse_path(const char *path, bool strict)
{
    bool was_slash;
    const char *p;
    const char **dirs, **dp;
    char *q;
    size_t path_len;
    size_t ndirs;
    int dropped;

    if (strict && *path != '/')
        return NULL;            /* Path not absolute */

    /*
     * Allocate space for the dirs list. The *maximum possible* number
     * equals the number of spans of non-slash characters.
     */
    was_slash = true;           /* Implicit leading slash */
    ndirs = 0;
    for (p = path; *p; p++) {
        if (!is_valid_char(*p))
            return NULL;        /* Invalid character in path */
        if (*p == '/') {
            was_slash = true;
        } else if (was_slash) {
            ndirs++;
            was_slash = false;
        }
    }
    path_len = p - path;

    dirs = dp = xmalloc((ndirs+1) * sizeof *dirs + path_len + 1);
    q = (char *)&dirs[ndirs+1];
    memcpy(q, path, path_len + 1);

    was_slash = true;
    while (*q) {
        if (*q == '/') {
            *q = '\0';
            if (!was_slash) {
                dropped = adjust_tail(&dp, dirs);
                if (dropped < 0 || (dropped && strict))
                    goto fail;
            }
            was_slash = true;
        } else if (was_slash) {
            *dp++ = q;
            was_slash = false;
        }
        q++;
    }
    if (!was_slash) {
        dropped = adjust_tail(&dp, dirs);
        if (dropped < 0 || (dropped && strict))
            goto fail;
    }

    *dp = NULL;
    return dirs;

fail:
    free(dirs);
    return NULL;
}

/*
 * Construct a canonical form path from a directory list, optionally
 * prepended with a path prefix. Returns a newly allocated string.
 */
char *build_path(const char *prefix, const char * const *dirs)
{
    size_t size = 1;            /* Space for NUL terminator */
    size_t prefix_len = 0;
    const char * const *dp;
    char *path, *q;

    if (prefix) {
        prefix_len = strlen(prefix);
        while (prefix_len && is_dir_separator(prefix[prefix_len-1]))
            prefix_len--;
    }
    size += prefix_len;

    for (dp = dirs; *dp; dp++) {
        size_t len = strlen(*dp);
        size += len+1;          /* / + string */
    }

    q = path = xmalloc(size);
    if (prefix_len) {
        q = mempcpy(q, prefix, prefix_len);
    }

    for (dp = dirs; *dp; dp++) {
        *q++ = '/';
        q = stpcpy(q, *dp);
    }

    /* If the dirs list is empty, stpcpy() won't have added a null byte */
    *q = '\0';
    return path;
}

/*
 * Compare two path lists.
 * Returns:
 *  0 on an explicit mismatch
 *  1 if the a is shorter but matches (is a prefix)
 *  2 if the b is shorter but matches (is a prefix)
 *  3 on an exact match
 */
unsigned int compare_paths(const char * const *a, const char * const *b)
{
    while (1) {
        if (!*a) {
            return *b ? 1 : 3;
        } else if (!*b) {
            return 2;
        } else if (strcmp(*a, *b)) {
            return 0;
        }

        a++;
        b++;
    }
}
