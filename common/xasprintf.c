/*
 * xvasprintf(), xasprintf()
 *
 * Error-checking versions of [v]asprintf()
 */

#include "xalloc.h"

#ifdef HAVE_VASPRINTF

int xvasprintf(char **strp, const char *fmt, va_list ap)
{
    int len;

    len = vasprintf(strp, fmt, ap);
    check_null(*strp);
    return len;
}

int xasprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vasprintf(strp, fmt, ap);
    va_end(ap);
    check_null(*strp);
    return len;
}

#else

int xvasprintf(char **strp, const char *fmt, va_list ap)
{
    size_t buf_size;
    int len;
    va_list ap2;
    char *str;

    va_copy(ap2, ap);
    len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);

    str = NULL;

    if (len >= 0) {
        /* Properly working vsnprintf() */
        buf_size = len + 1;
    } else {
        /* Old, broken vsnprintf() */
        buf_size = BUFSIZ;      /* As good as any guess? */
    }

    for (;;) {
        str = xrealloc(str, buf_size);
        va_copy(ap2, ap);
        len = vsnprintf(str, buf_size, fmt, ap2);
        va_end(ap2);

        if (likely((unsigned int)len < buf_size))
            break;          /* Good! */

        if (buf_size >= INT_MAX) {
            xfree(str);
            xalloc_out_of_memory(); /* WHAT THE HELL... */
            abort();
        }

        buf_size <<= 1;
        if (buf_size > INT_MAX)
            buf_size = INT_MAX;
    }

    /* Give a little bit of a slack before bothering with realloc() */
    if (unlikely(buf_size > (size_t)len + 4*sizeof(void *)))
        str = xrealloc(str, (size_t)len + 1);

    *strp = str;
    return len;
}

int xasprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = xvasprintf(strp, fmt, ap);
    va_end(ap);
    return len;
}

#endif
