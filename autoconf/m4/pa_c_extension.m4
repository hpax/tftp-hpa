dnl --------------------------------------------------------------------------
dnl PA_C_EXTENSION
dnl
dnl Test to see if "__extension__" exists, and define the macro "extension"
dnl accordingly, to flag extensions that are explicitly enabled under
dnl conditionals.
dnl --------------------------------------------------------------------------
AC_DEFUN_ONCE([_PA_C_EXTENSION],
[AC_REQUIRE([AC_PROG_CC])
 AC_BEFORE([AC_USE_SYSTEM_EXTENSIONS],[$0])
 AC_CACHE_CHECK([if $CC has a way to flag standards extensions], [pa_cv_c_extension],
 [pa_cv_c_extension=no
# This is written as a loop to make it easier to add other variants
  for pa_extension_try in __extension__
 do
  AS_IF([test x$pa_cv_c_extension = xno],
        [AC_COMPILE_IFELSE([AC_LANG_SOURCE([
$pa_extension_try unsigned long dummy;
        ])], [pa_cv_c_extension="$pa_extension_try"])])
 done
 ])])

AC_DEFUN([PA_C_EXTENSION],
[AC_REQUIRE([_PA_C_EXTENSION])
 AS_CASE([x$pa_cv_c_extension],
 [xno], [pa_extension=""],
 [*],   [pa_extension="$pa_cv_c_extension"])
AC_DEFINE_UNQUOTED([extension], [$pa_extension],
  [Define to a keyword to flag C standard extensions, or empty.])
])
