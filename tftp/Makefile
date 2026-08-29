# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 1999-2006 H. Peter Anvin
# Copyright (c) 2024-2025 Intel Corporation; Author: H. Peter Anvin

SRCROOT = ..
VERSION = $(shell cat ../version)

-include ../config/MCONFIG
include ../MRULES

OBJS = tftp.$(O) main.$(O)

all: tftp$(X) tftp.1

tftp$(X): $(OBJS) $(filter %.a,$(TFTP_LIBS))
	$(CC) $(CFLAGS) $(LDFLAGS) -o tftp$(X) $(OBJS) $(TFTP_LIBS)

tftp.1: tftp.1.in ../version
	sed -e 's/@@VERSION@@/$(VERSION)/g' < tftp.1.in > tftp.1

install: all
	mkdir -p $(INSTALLROOT)$(BINDIR) $(INSTALLROOT)$(MANDIR)/man1
	$(INSTALL_PROGRAM) tftp$(X) $(INSTALLROOT)$(BINDIR)
	$(INSTALL_DATA)    tftp.1   $(INSTALLROOT)$(MANDIR)/man1

clean:
	rm -f tftp tftp.1
