# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 1999-2006 H. Peter Anvin
# Copyright (c) 2024-2026 Intel Corporation; Author: H. Peter Anvin

SRCROOT = ..
VERSION = $(shell cat ../version)

-include ../config/MCONFIG
include ../MRULES

OBJS = tftpd.$(O) recvfrom.$(O) misc.$(O) path.$(O) listen.$(O) $(TFTPDOBJS)

all: tftpd$(X) tftpd.8

tftpd$(X): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o tftpd$(X) $(OBJS) $(TFTPD_LIBS)

$(OBJS): ../common/tftpsubs.h

tftpd.8: tftpd.8.in ../version
	sed -e 's/@@VERSION@@/$(VERSION)/g' < tftpd.8.in > tftpd.8

install: all
	mkdir -p $(INSTALLROOT)$(SBINDIR) $(INSTALLROOT)$(MANDIR)/man8
	$(INSTALL_PROGRAM) tftpd$(X) $(INSTALLROOT)$(SBINDIR)/in.tftpd
	$(INSTALL_DATA)    tftpd.8 $(INSTALLROOT)$(MANDIR)/man8/in.tftpd.8
	cd $(INSTALLROOT)$(MANDIR)/man8 && $(LN_S) -f in.tftpd.8 tftpd.8

clean:
	rm -f tftpd tftpsubs.c tftpsubs.h tftpd.8
