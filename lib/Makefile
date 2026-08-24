# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2001-2005 H. Peter Anvin
# Copyright (c) 2024 Intel Corporation; Author: H. Peter Anvin

#
# Extra functions which may not be available everywhere
#

SRCROOT = ..

-include ../config/MCONFIG
include ../MRULES

ifeq ($(LIBOBJS),)
all:
else
all:	libxtra.a
endif

install:

libxtra.a: $(LIBOBJS)
	-rm -f libxtra.a
	$(AR) libxtra.a $(LIBOBJS)
	$(RANLIB) libxtra.a
