/* SPDX-License-Identifier: BSD-3-Clause-UC */
/* Copyright (c) 2026 H. Peter Anvin <hpa@zytor.com> */

/*
 * This header avoids relying on <arpa/tftp.h> from the system.
 */

#ifndef TFTP_TFTP_H
#define TFTP_TFTP_H

#include "config.h"

/* TFTP packet type */
enum tftp_opcode {
    NULL_OP,                    /* undefined opcode */
    RRQ,                        /* read request */
    WRQ,                        /* write request */
    DATA,                       /* data packet */
    ACK,                        /* data acknowledgement */
    ERROR,                      /* error, terminate transfer */
    OACK,                       /* option acknowledgement */
    PTYPE_CNT                   /* number of defined packet types */
};

/* TFTP ERROR failure code */
enum tftp_error {
    EUNDEF,                     /* not defined/generic error */
    ENOTFOUND,                  /* file not found */
    EACCESS,                    /* access violation */
    ENOSPACE,                   /* disk full or allocation exceeded */
    EBADOP,                     /* illegal TFTP operation */
    EBADID,                     /* unknown transfer ID */
    EEXISTS,                    /* file already exists */
    ENOUSER,                    /* no such user */
    EOPTNEG,                    /* option negotiation failure */
    ETYPE_CNT                   /* number of defined errors */
};

struct tftphdr {
    uint16_t th_opcode;
    union {
        char th_stuff[];
        struct {
            uint16_t th_block;
            char th_data[];
        };
        struct {
            uint16_t th_code;
            char th_msg[];
        };
    };
};

/* Range of possible block sizes (see RFC 1350 and RFC 2348) */

#define MIN_SEGSIZE	8       /* minimum permitted by spec */
#define SEGSIZE		512     /* blksize default (if not negotiated) */
#define MAX_SEGSIZE	65464   /* maximum permitted by spec */

/* TFTP default port */
#define TFTP_IP_PORT	69

/* Known protocol options. */
/* Keep this in sync with enum protocol_options in common/tftp.h. */
enum protocol_option_enum {
    PO_BLKSIZE,
    PO_BLKSIZE2,
    PO_ROLLOVER,
    PO_TIMEOUT,
    PO_TSIZE,
    PO_UTIMEOUT,
    PO_WINDOWSIZE,

    PO_NUM_OPTS
};

#endif /* TFTP_TFTP_H */
