/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 * All rights reserved.
 */

#ifndef TFTP_STRLIST_H
#define TFTP_STRLIST_H 1

#include "config.h"

struct liststr {
    struct liststr *next;
    size_t len;
    char str[1];
};
struct strlist {
    struct liststr *list;
    struct liststr **endp;
};

static inline struct strlist *strlist_init(struct strlist *list)
{
    list->list = NULL;
    list->endp = &list->list;
    return list;
}

static inline bool strlist_isempty(const struct strlist *list)
{
    return !list->list;
}

struct liststr *strlist_add(struct strlist *list, const char *str);
struct strlist *strlist_free(struct strlist *);

#endif
