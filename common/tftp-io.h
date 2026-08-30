/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 */

#ifndef TFTP_IO_H
#define TFTP_IO_H

#include "tftp-xfer.h"

struct tftp_io;

struct tftp_io *tftp_io_reader_start(FILE *, int, unsigned int,
                                     unsigned int, unsigned int, int);
struct tftp_io *tftp_io_writer_start(FILE *, int, unsigned int,
                                     unsigned int, int);
void tftp_io_stop(struct tftp_io *);

extern const struct tftp_xfer_io_ops tftp_io_xfer_ops;

#endif /* TFTP_IO_H */
