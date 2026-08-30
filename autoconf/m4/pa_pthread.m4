dnl --------------------------------------------------------------------------
dnl PA_PTHREAD
dnl
dnl  Look for threads and define HAVE_PTHREAD if available, adding
dnl  the appropriate options to *FLAGS.
dnl --------------------------------------------------------------------------
AC_DEFUN([PA_PTHREAD],
[AC_CHECK_HEADERS_ONCE([pthread.h])
 AS_IF([test "x$ac_cv_header_pthread_h" = xyes],
 [PA_ADD_LANGFLAGS([-pthread], [-pthreads])
  AC_SEARCH_LIBS(pthread_create, [pthread pthreads])
  AC_MSG_CHECKING([that POSIX threads work])
  AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([[
#include <pthread.h>
]],
    [[pthread_t thread;
      return pthread_create(&thread, NULL, NULL, NULL);]])],
    [AC_MSG_RESULT([yes])
     AC_DEFINE([HAVE_PTHREAD], 1,
      [Define to 1 if your have working POSIX threads.])],
    [AC_MSG_RESULT([no])])])])
