# Copilot instructions

## Build, configure, and clean

This is a portable C project built with GNU make and Autoconf. The normal in-tree workflow is:

```sh
./configure
make
```

Use `./configure --help` for the complete option list. Important package-specific switches include:

```sh
./configure --without-tcpwrappers --without-remap --without-readline
./configure --disable-ipv6
./configure --enable-debug --enable-profiling --enable-lto --enable-sanitizer
./configure --enable-werror
```

Set `CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, or `LIBS` on the `configure` command line when targeting a non-default compiler or dependency location. The default installation prefix is `/usr`; the client is installed under `bindir` and `in.tftpd` under `sbindir`. Packaging builds can use `make INSTALLROOT=/path install`.

Useful targets:

```sh
make                  # build lib, common, tftp, and tftpd
make SUB=tftp         # build only selected top-level subdirectories
make clean
make distclean
make install
```

`configure` generates `config/MCONFIG` and `config/config.h`; the build also generates `version.h` and rendered man pages. Do not hand-edit generated files. If changing `configure.ac` or Autoconf macros, regenerate with `make autoconf` (or `./autogen.sh` as appropriate) and review generated changes.

## Tests

The available automated test is an integration smoke test for bidirectional transfers. Build first, then run:

```sh
./tests/test-tftp.sh ./tftpd/tftpd ./tftp/tftp
```

The optional third argument selects the UDP port, for example:

```sh
./tests/test-tftp.sh ./tftpd/tftpd ./tftp/tftp 6970
```

The script starts standalone `tftpd` on localhost, exercises downloads and uploads for text and binary files, compares the results, and cleans up temporary data. It accepts custom client/server paths; when omitted, it defaults to the binaries in this tree. There is no separate unit-test runner or per-case selector.

## Architecture

- `tftp/` contains the interactive and command-line TFTP client. `main.c` parses options and implements the command interface; `tftp.c` implements RRQ/WRQ transfer loops, retransmission handling, packet tracing, and netascii/octet file I/O.
- `tftpd/` contains the server daemon. `tftpd.c` handles request dispatch, option negotiation, privilege/security modes, logging, and per-transfer processing. `listen.c` and `recvfrom.c` handle listening sockets, local-address discovery, and platform-specific UDP behavior. `path.c` validates and canonicalizes requested paths. `remap.c` implements optional regex-based filename remapping.
- `common/` builds `libcommon.a`, shared by both programs. It provides buffered read-ahead/write-behind transfer I/O, timeout packet draining, address helpers, allocation wrappers, program-name handling, and the select/poll abstraction.
- `lib/` supplies portability fallbacks and compatibility routines selected by configure checks. `config/` contains Autoconf templates and generated configuration definitions. `autoconf/` contains project-specific Autoconf macros and helper scripts.
- The top-level `Makefile` delegates to the subdirectory Makefiles in dependency order: `lib` and `common` support both executables, then `tftp` and `tftpd` link against `common/libcommon.a`. `MRULES` provides the shared compilation and dependency rules.

The server supports both inetd-style invocation and standalone mode. Security-sensitive behavior is concentrated in `tftpd/path.c` and the access logic in `tftpd/tftpd.c`: secure/chroot mode, Unix-permission mode, file creation, user/group dropping, path restrictions, and optional remapping must remain consistent when changing request handling.

## Repository conventions

- Preserve the existing Autoconf portability model. Add platform-dependent behavior through configure checks/macros and generated `config/config.h` definitions rather than assuming a specific Unix API.
- Include `config.h` first in server/platform-sensitive sources where the existing files do so, and use the shared types/helpers in `common/tftpsubs.h` for socket addresses, packet sizing, allocation, and transfer buffering.
- Keep client and server protocol changes aligned: both implement TFTP packet framing, retransmission/synchronization, option negotiation, and netascii/octet semantics, while sharing buffering primitives from `common/`.
- TFTP packet buffers must account for negotiated block sizes (`MAX_SEGSIZE + 4`); do not reuse the client's smaller request buffer for full negotiated data packets.
- Preserve the project’s C style and portability constraints: four-space indentation in C, tabs at width eight where applicable, LF endings, and the existing SPDX header/copyright conventions. Use the compiler warning set configured by `configure.ac`; `--enable-werror` is available for stricter builds.
- Runtime and security changes should be checked against `README.security`, the relevant manual page source (`tftp/*.1.in` or `tftpd/*.8.in`), and the integration script. Remapping behavior and its rule syntax are daemon features, not client features.
