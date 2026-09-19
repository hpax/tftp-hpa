/*
 * Convert a TFTP error packet safely printable string by converting
 * control characters to spaces, limiting the length, and including
 * an explanation of the error code.
 *
 * Returns a newly allocated buffer.
 */

#include "config.h"
#include "tftpsubs.h"

const char * const packet_types[PTYPE_CNT] = {
    "null",
    "RRQ",
    "WRQ",
    "DATA",
    "ACK",
    "ERROR",
    "OACK"
};

const char * const errmsgs[ETYPE_CNT] = {
    "unspecified error",		/* 0 - EUNDEF */
    "file not found",                   /* 1 - ENOTFOUND */
    "access denied",                    /* 2 - EACCESS */
    "insufficient space for upload",    /* 3 - ENOSPACE */
    "invalid TFTP operation",           /* 4 - EBADOP */
    "unknown transfer ID",              /* 5 - EBADID */
    "file already exists",              /* 6 - EEXISTS */
    "no such user",                     /* 7 - ENOUSER */
    "options negotiation failure"       /* 8 - EOPTNEG */
};

const char *error_msg(int err)
{
    const char *msg;

    if (err < 0) {
        int xerr = errno;       /* In case strerror() mucks with errno */
        msg = strerror(-err);
        errno = xerr;
    } else if (err < ETYPE_CNT) {
        msg = errmsgs[err];
    } else {
        msg = "";
    }

    return msg;
}

char *errpkt_to_string(const struct tftphdr *tp, int n)
{
    const char *p, *ep;
    char *msg;
    uint16_t opcode = ntohs(tp->th_opcode);
    uint16_t nerr;

    if (n < 2)
        return xstrdup("packet too short");

    if (opcode != ERROR) {
        if (opcode >= PTYPE_CNT)
            xasprintf(&msg, "invalid packet type %u", opcode);
        else
            xasprintf(&msg, "unexpected %s packet", packet_type(opcode));
        return msg;
    }

    if (n < 4)
        return xstrdup("error packet too short");

    nerr = ntohs(tp->th_code);

    /* Truncate a message at control characters or end of packet */
    p = tp->th_msg;
    ep = (const char *)tp + n;
    while (p < ep) {
        if (*p < ' ' || *p == 127)
            break;
        p++;
    }

    /*
     * Emit an ellipsis if the message was truncated somehow (the string
     * should be null-terminated in the last position in the packet)
     */
    ep = (p != ep-1 || *p) ? "..." : "";
    n = p - tp->th_msg;
    if (n) {
        p = tp->th_msg;
    } else {
        static const char no_msg[] = "(no message)";
        p = no_msg;
        n = sizeof no_msg;
    }

    if (nerr >= ETYPE_CNT)
        xasprintf(&msg, "unknown error %u: %*s%s", nerr, n, p, ep);
    else
        xasprintf(&msg, "%s: %*s%s", errmsgs[nerr], n, p, ep);

    return msg;
}

/*
 * Create an error packet. Return its length and set *tpp to the
 * newly allocated packet.
 */
int make_errpacket(struct tftphdr **tpp, int error, const char *msg)
{
    struct tftphdr *tp;
    int length;

    if (!msg)
        msg = error_msg(error);

    /* Cap the error string to 512 bytes */
    length = strnlen(msg, SEGSIZE-1);

    tp = xmalloc(length + 5);   /* 4 byte header + NUL */

    /* Convert negative errno to TFTP error codes */
    switch (error) {
    case -ENOENT:
    case -ENOTDIR:
    case -EPERM:
        error = ENOTFOUND;
        break;
    case -ENOSPC:
        error = ENOSPACE;
        break;
    case -EEXIST:
        error = EEXISTS;
        break;
    default:
        if (error < 0 || !*msg)
            error = EUNDEF;
        break;
    }

    tp->th_opcode = htons(ERROR);
    tp->th_code   = htons(error);
    memcpy(tp->th_msg, msg, length);
    tp->th_msg[length] = 0;

    *tpp = tp;
    return length + 5;          /* Include header and NUL */
}
