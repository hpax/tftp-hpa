# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2006 H. Peter Anvin
# Copyright (c) 2024-2026 Intel Corporation; Author: H. Peter Anvin

SRCROOT = ..
VERSION = $(shell cat ../version)

-include ../config/MCONFIG
include ../MRULES

OBJS = tftpsubs.$(O) tftp-xfer.$(O) tftp-io.$(O) signal.$(O) xalloc.$(O) \
	progname.$(O) pollset.$(O)
LIB  = libcommon.a

all: $(LIB)

$(LIB): $(OBJS)
	-rm -f $(LIB)
	$(AR) $(LIB) $(OBJS)
	$(RANLIB) $(LIB)

$(OBJS): tftpsubs.h
tftp-xfer.$(O): tftp-xfer.h
tftp-io.$(O): tftp-io.h tftp-xfer.h

install:

clean:
	rm -f $(LIB)
