/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 * All rights reserved.
 */

#include "config.h"
#include "common/tftpsubs.h"           /* xmalloc()/xfree() */
#include "strlist.h"

struct liststr *strlist_add(struct strlist *list, const char *str)
{
    size_t len = strlen(str);
    struct liststr *ls;

    /* sizeof(*ls) includes space for the final NUL */
    ls = xmalloc(sizeof(*ls) + len);
    ls->next = NULL;
    ls->len = len;
    memcpy(ls->str, str, len+1);

    *list->endp = ls;
    list->endp = &ls->next;

    return ls;
}

struct strlist *strlist_free(struct strlist *list)
{
    struct liststr *ls, *next;

    for (ls = list->list; ls; ls = next) {
        next = ls->next;
        xfree(ls);
    }

    return strlist_init(list);
}
