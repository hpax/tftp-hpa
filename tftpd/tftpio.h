/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Intel Corporation; Author: H. Peter Anvin
 */

#ifndef TFTPD_TFTPIO_H
#define TFTPD_TFTPIO_H

#include "config.h"

#ifdef HAVE_PTHREAD

struct tftphdr;
struct tftpio;

struct tftpio *tftpio_reader_start(FILE *, int, unsigned int, unsigned int);
int tftpio_reader_window(struct tftpio *, unsigned int *, int *);
struct tftphdr *tftpio_reader_packet(struct tftpio *, unsigned int);
int tftpio_reader_length(struct tftpio *, unsigned int);
void tftpio_reader_release(struct tftpio *);

struct tftpio *tftpio_writer_start(FILE *, int, unsigned int);
struct tftphdr *tftpio_writer_reserve(struct tftpio *);
void tftpio_writer_publish(struct tftpio *, int);
int tftpio_writer_drain(struct tftpio *);

void tftpio_stop(struct tftpio *);

#endif /* HAVE_PTHREAD */
#endif /* TFTPD_TFTPIO_H */
