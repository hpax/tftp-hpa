/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Intel Corporation; Author: H. Peter Anvin
 */

#ifndef TFTPD_TFTPIO_H
#define TFTPD_TFTPIO_H

#include "config.h"
#include "common/tftp-xfer.h"

#ifdef HAVE_PTHREAD

struct tftpio;

struct tftpio *tftpio_reader_start(FILE *, int, unsigned int, unsigned int,
                                   unsigned int);
struct tftpio *tftpio_writer_start(FILE *, int, unsigned int, unsigned int);
void tftpio_stop(struct tftpio *);

extern const struct tftp_xfer_io_ops tftpio_xfer_io_ops;

#endif /* HAVE_PTHREAD */
#endif /* TFTPD_TFTPIO_H */
