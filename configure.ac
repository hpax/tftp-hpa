dnl SPDX-License-Identifier: BSD-3-Clause
dnl
dnl Copyright (c) 1999-2008 H. Peter Anvin
dnl Copyright (c) 2008 Karsten Keil
dnl Copyright (c) 2011-2026 Intel Corporation; Author: H. Peter Anvin
dnl
dnl Process this file with autoconf 2.71 or later to produce
dnl a configure script.
AC_PREREQ([2.71])
AC_INIT
AC_CONFIG_SRCDIR([MRULES])
AC_PREFIX_DEFAULT([/usr])
AC_CONFIG_AUX_DIR([autoconf/helpers])

dnl This prevents us from running Wine and thinking we are not
dnl cross-compiling when in fact we are; running Wine here is at
dnl the best very slow and doesn't buy us a single thing at all.
PA_CROSS_COMPILE

dnl Enable any available C extensions
PA_PROG_CC
AC_USE_SYSTEM_EXTENSIONS

dnl Options for debugging and profiling
PA_OPTION_DEBUG
PA_OPTION_PROFILING

dnl LLVM doesn't error out on invalid -W options unless this option is
dnl specified first.  Enable this so this script can actually discover
dnl which -W options are possible for this compiler.
PA_ADD_CFLAGS([-Werror=unknown-warning-option])

dnl Force gcc and gcc-compatible compilers treat signed integers
dnl as 2's complement
PA_ADD_CFLAGS([-fwrapv])

dnl Force clang to behave in a predictable manner, in order to make bugs
dnl possible to track down. gcc appears to have this behavior by default.
PA_ADD_CFLAGS([-ftrivial-auto-var-init=zero])

dnl Some environments abuse __STRICT_ANSI__ to disable some
dnl function declarations
PA_ADD_CFLAGS([-U__STRICT_ANSI__])

dnl Don't put things in common if we can avoid it.  We don't want to
dnl assume all compilers support common, and this will help find those
dnl problems.  This also works around an OSX linker problem.
PA_ADD_CFLAGS([-fno-common])

dnl Tests which may trigger warnings on some compilers
AC_C_CONST
AC_C_INLINE
AC_C_RESTRICT

dnl Checks for header files.
AC_CHECK_INCLUDES_DEFAULT

dnl See if we need extra libraries
XTRA=false

AC_SEARCH_LIBS([strerror],[cposix])

AC_CHECK_HEADERS_ONCE(grp.h)
AC_CHECK_HEADERS_ONCE(inttypes.h)
AC_CHECK_HEADERS_ONCE(libgen.h)
AC_CHECK_HEADERS_ONCE(locale.h)
AC_CHECK_HEADERS_ONCE(paths.h)
AC_CHECK_HEADERS_ONCE(poll.h)
AC_CHECK_HEADERS_ONCE(setjmp.h)
AC_CHECK_HEADERS_ONCE(stdint.h)
AC_CHECK_HEADERS_ONCE(stdnoreturn.h)
AC_CHECK_HEADERS_ONCE(strings.h)
AC_CHECK_HEADERS_ONCE(sysexits.h)
AC_CHECK_HEADERS_ONCE(unistd.h)
AC_CHECK_HEADERS_ONCE(sys/filio.h)
AC_CHECK_HEADERS_ONCE(sys/ioctl.h)
AC_CHECK_HEADERS_ONCE(sys/select.h)
AC_CHECK_HEADERS_ONCE(sys/stat.h)
AC_CHECK_HEADERS_ONCE(sys/time.h)
PA_CHECK_INTTYPES_H_SANE

dnl This is needed on some versions of FreeBSD...
AC_CHECK_HEADERS_ONCE(machine/param.h)

dnl Windows...
PA_ADD_HEADERS(windows.h)
PA_ADD_HEADERS(winsock2.h)
AS_IF([test "x$ac_cv_header_winsock2_h" != xyes],
      [PA_ADD_HEADERS(winsock.h)])

PA_ADD_HEADERS(fcntl.h)
PA_ADD_HEADERS(sys/types.h)
PA_ADD_HEADERS(arpa/inet.h)
PA_ADD_HEADERS(sys/socket.h)
PA_ADD_HEADERS(sys/file.h)
PA_ADD_HEADERS(netinet/in.h)
PA_ADD_HEADERS(sys/uio.h)
PA_ADD_HEADERS(netdb.h)

AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_MODE_T
AC_TYPE_SIZE_T
AC_CHECK_TYPES(intmax_t)
AC_CHECK_TYPES(long long)
AC_CHECK_TYPES(uint16_t)
AC_CHECK_TYPES(uint32_t)
AC_CHECK_TYPES(u_short)
AC_CHECK_TYPES(u_long)

AC_CHECK_TYPES(socklen_t)

AC_SEARCH_LIBS(socket, [socket ws2_32 wsock32], ,
 [AC_MSG_ERROR(socket library not found)])

PA_ARG_DISABLED([poll],
  [use select() instead of poll(): for debugging only], [],
  [
	AC_CHECK_FUNCS(poll)
	AC_CHECK_FUNCS(ppoll)
  ])
AC_CHECK_FUNCS(fcntl)
AC_CHECK_FUNCS(flock)
AC_CHECK_FUNCS(setsid)
AC_CHECK_FUNCS(recvmsg)
AC_CHECK_FUNCS(ftruncate)
AC_CHECK_FUNCS(setresuid)
AC_CHECK_FUNCS(setreuid)
AC_CHECK_FUNCS(setresgid)
AC_CHECK_FUNCS(setregid)
AC_CHECK_FUNCS(initgroups)
AC_CHECK_FUNCS(setgroups)
AC_CHECK_TYPES(sighandler_t)

dnl Solaris 8 has [u]intmax_t but not strtoumax().  How utterly braindamaged.
AC_CHECK_FUNCS(strtoumax)
AC_CHECK_FUNCS(strtoull)

AC_CHECK_MEMBERS(struct msghdr.msg_control)
AC_CHECK_MEMBERS(struct in_pktinfo.ipi_addr)
AC_CHECK_MEMBERS(struct addrinfo.ai_addr)

AC_CHECK_DECLS([O_NONBLOCK, O_NDELAY, O_BINARY, O_TEXT])
AC_CHECK_DECLS([F_SETLK])
AC_CHECK_DECLS([LOCK_SH, LOCK_EX])

PA_ADD_HEADERS(wctype.h)
PA_ADD_HEADERS(wchar.h)
AC_CHECK_FUNCS(mbrtowc)
AC_CHECK_FUNCS(towlower)

PA_SIGSETJMP

dnl
dnl Get common paths
dnl
SRCROOT=`cd $srcdir && pwd`
OBJROOT=`pwd`

PA_SEARCH_LIBS_AND_ADD(getopt_long, getopt, getopt_long)
PA_SEARCH_LIBS_AND_ADD(getaddrinfo, [nsl resolv])
AS_IF([$pa_add_getaddrinfo],
[AC_SEARCH_LIBS(gethostbyname, [nsl resolv],
      [AC_SEARCH_LIBS(herror, [nsl resolv], ,
	 [AC_MSG_ERROR(herror not found)])],
      [AC_MSG_ERROR(gethostbyname not found)])],
[AC_SEARCH_LIBS(freeaddrinfo, [nsl resolv], ,
      [AC_MSG_ERROR(getaddrinfo but not freeaddrinfo found)])
    AC_SEARCH_LIBS(gai_strerror, [nsl resolv], ,
      [AC_MSG_ERROR(getaddrinfo but not gai_strerror found)])])

PA_SEARCH_LIBS_AND_ADD(inet_ntop, [nsl resolv])
AS_IF([$pa_add_inet_ntop],
  [AC_SEARCH_LIBS(inet_ntoa, [nsl resolv], ,
   [AC_MSG_ERROR(inet_ntoa not found)])])

AC_SEARCH_LIBS(inet_aton, [nsl resolv], ,[AC_MSG_ERROR(inet_aton not found)])

PA_SEARCH_LIBS_AND_ADD(daemon)
AC_SEARCH_LIBS(dup2, , ,[AC_MSG_ERROR(dup2 not found; the bundled ersatz dup2() implementation has been removed as obsolete)])

AS_IF([$XTRA], [XTRALIBS="$OBJROOT/lib/libxtra.a $XTRALIBS"])

dnl
dnl These libraries apply to the server only
dnl

common_libs="$LIBS"

AC_CHECK_DECLS(IPPORT_TFTP)
AC_CHECK_HEADERS_ONCE([pthread.h])
AS_IF([test "x$ac_cv_header_pthread_h" = xyes], [
  pthread_save_CFLAGS=$CFLAGS
  pthread_save_LIBS=$LIBS
  PTHREAD_CFLAGS=
  PTHREAD_LIBS=
  pthread_found=no
  AC_MSG_CHECKING([for POSIX threads])
  for pthread_flag in none -pthread -pthreads -lpthread -lthread; do
    CFLAGS=$pthread_save_CFLAGS
    LIBS=$pthread_save_LIBS
    case $pthread_flag in
      none) ;;
      -pthread|-pthreads)
        CFLAGS="$CFLAGS $pthread_flag"
        LIBS="$LIBS $pthread_flag"
        ;;
      *) LIBS="$LIBS $pthread_flag" ;;
    esac
    AC_LINK_IFELSE(
      [AC_LANG_PROGRAM([[#include <pthread.h>]],
                       [[pthread_t thread;
                         return pthread_create(&thread, NULL, NULL, NULL);]])],
      [pthread_found=yes
       case $pthread_flag in
         none) ;;
         -pthread|-pthreads)
           PTHREAD_CFLAGS=$pthread_flag
           PTHREAD_LIBS=$pthread_flag
           ;;
         *) PTHREAD_LIBS=$pthread_flag ;;
       esac
       break])
  done
  CFLAGS=$pthread_save_CFLAGS
  LIBS=$pthread_save_LIBS
  AS_IF([test "$pthread_found" = yes], [
    AC_MSG_RESULT([yes])
    AC_DEFINE([HAVE_PTHREAD], 1,
      [Define to 1 if the server can use POSIX threads.])
  ], [AC_MSG_RESULT([no])])
])
AC_SUBST([PTHREAD_CFLAGS])
AC_SUBST([PTHREAD_LIBS])

AC_ARG_VAR([TFTPD_MAX_WINDOWSIZE],
 [maximum RFC 7440 windowsize accepted by tftpd (default: 256)])
AS_IF([test -z "$TFTPD_MAX_WINDOWSIZE"], [TFTPD_MAX_WINDOWSIZE=256])
AS_CASE([$TFTPD_MAX_WINDOWSIZE],
 [*[!0-9]*|''], [AC_MSG_ERROR([TFTPD_MAX_WINDOWSIZE must be a positive integer])])
AS_IF([test "$TFTPD_MAX_WINDOWSIZE" -lt 1 || test "$TFTPD_MAX_WINDOWSIZE" -gt 32768],
 [AC_MSG_ERROR([TFTPD_MAX_WINDOWSIZE must be in the range 1-32768])])
AC_DEFINE_UNQUOTED([MAX_WINDOWSIZE], [$TFTPD_MAX_WINDOWSIZE],
 [maximum RFC 7440 windowsize accepted by tftpd])

AC_ARG_VAR([TFTPD_MAX_WINDOWBYTES],
 [maximum bytes in an RFC 7440 tftpd window; 0 is unlimited (default: 4194304)])
AS_IF([test -z "$TFTPD_MAX_WINDOWBYTES"], [TFTPD_MAX_WINDOWBYTES=4194304])
AS_CASE([$TFTPD_MAX_WINDOWBYTES],
 [*[!0-9]*|''], [AC_MSG_ERROR([TFTPD_MAX_WINDOWBYTES must be a nonnegative integer])])
AC_DEFINE_UNQUOTED([MAX_WINDOWBYTES], [$TFTPD_MAX_WINDOWBYTES],
 [maximum bytes in an RFC 7440 tftpd window; zero is unlimited])

AC_CHECK_HEADERS_ONCE([regex.h])
PA_ARG_DISABLED([remap],
 [disable regex-based filename remapping], [],
 [AS_IF([test x"$ac_cv_header_regex_h" = xyes],
  [AC_SEARCH_LIBS(regcomp, [regex rx],
   [AC_DEFINE([WITH_REGEX], 1,
	     [Define if we are compiling with regex filename remapping.])
    TFTPDOBJS="remap.\$(O) $TFTPOBJS"])])])

TFTPD_LIBS="$LIBS $XTRALIBS"
LIBS="$common_libs"

dnl
dnl These libraries apply to the client only
dnl

AH_TEMPLATE([WITH_READLINE],
[Define if we are compiling with readline/editline command-line editing.])

PA_ARG_DISABLED([readline],
 [disable the use of readline command-line editing], [],
 [
  AC_CHECK_HEADER([readline/readline.h],
	[
		dnl readline may need libtermcap or somesuch...
		AC_SEARCH_LIBS(tputs, [termcap terminfo])

		AC_SEARCH_LIBS(readline, [readline history],
		[AC_DEFINE(WITH_READLINE)])
		AC_CHECK_HEADERS(readline/history.h)
	],
	[AC_CHECK_HEADER([editline/readline.h],
	[
		dnl editline may need libtermcap or somesuch...
		AC_SEARCH_LIBS(tputs, [termcap terminfo])

		AC_SEARCH_LIBS(editline, [edit],
		[AC_DEFINE(WITH_READLINE)])
	])])
],:)

TFTP_LIBS="$LIBS $XTRALIBS"
LIBS="$common_libs"

dnl
dnl   Check for IPV6 and disable-ipv6
dnl

AC_CHECK_MEMBERS(struct sockaddr_in6.sin6_addr)
AC_MSG_CHECKING([for IPv6 support])
PA_ARG_DISABLED([ipv6],
 [disable support for IPv6],
 [AC_MSG_RESULT(disabled)],
 [AS_IF([test x"$ac_cv_member_struct_sockaddr_in6_sin6_addr$ac_cv_member_struct_addrinfo_ai_addr" = xyesyes],
 [AC_MSG_RESULT(yes)
  AC_DEFINE(HAVE_IPV6, 1, [define if IPv6 support is enabled])
  AC_CHECK_MEMBERS(struct in6_pktinfo.ipi6_addr)],
 [AC_MSG_RESULT(no)
  AC_MSG_WARN([*** we do not have required IPv6 structs - IPv6 will be disabled])])])

AC_SUBST(SRCROOT)
AC_SUBST(OBJROOT)

AC_SUBST(TFTP_LIBS)
AC_SUBST(TFTPD_LIBS)
AC_SUBST(TFTPDOBJS)

AC_PROG_LN_S
AC_PROG_RANLIB

dnl
dnl Make sure the install program has an absolute path if it
dnl has a path at all.  autoconf doesn't do this "in order
dnl to not pollute the cache."  Sigh.
dnl Note: the $ needs to be double-quoted for reasons unknown.
dnl
AC_PROG_INSTALL
[if echo "$INSTALL" | grep '^[^/].*/' > /dev/null 2>&1; then
   INSTALL='\${SRCROOT}'/"$INSTALL"
fi]

dnl Support for function garbage collection, LTO, and sanitizers
PA_OPTION_LTO
PA_OPTION_GC
PA_OPTION_SANITIZER

PA_ARG_ENABLED([werror],
 [compile with -Werror to error out on any warning],
 [PA_ADD_CFLAGS([-Werror])])

PA_ADD_CFLAGS(-W)
PA_ADD_CFLAGS(-Wall)
PA_ADD_CFLAGS(-Wpointer-arith)
PA_ADD_CFLAGS(-Wbad-function-cast)
PA_ADD_CFLAGS(-Wcast-equal)
PA_ADD_CFLAGS(-Wstrict-prototypes)
PA_ADD_CFLAGS(-Wmissing-prototypes)
PA_ADD_CFLAGS(-Wmissing-declarations)
PA_ADD_CFLAGS(-Wnested-externs)
PA_ADD_CFLAGS(-Winline)
PA_ADD_CFLAGS(-Wwrite-strings)
PA_ADD_CFLAGS(-Wundef)
PA_ADD_CFLAGS(-Wshadow)
PA_ADD_CFLAGS(-Wsign-compare)
PA_ADD_CFLAGS(-fno-strict-aliasing)

dnl
dnl Test compiler features. On some compilers, this can be affected
dnl by -Werror options, so run this *after* those options are added.
dnl
PA_CHECK_BAD_STDC_INLINE
PA_C_TYPEOF
PA_DEPEND

AC_CONFIG_HEADERS([config/config.h])
AC_CONFIG_FILES([config/MCONFIG])
AC_OUTPUT
