dnl --------------------------------------------------------------------------
dnl PA_C_NORETURN
dnl
dnl  How to declare a noreturn function
dnl --------------------------------------------------------------------------

AC_DEFUN_ONCE([_PA_C_NORETURN],
[AC_CACHE_CHECK([how $CC declares noreturn functions], [pa_cv_c_noreturn],
 [AC_REQUIRE([AC_PROG_CC])
 AC_BEFORE([AC_USE_SYSTEM_EXTENSIONS],[$0])
 AC_REQUIRE([PA_FN_TRIM_WHITESPACE])
 AC_REQUIRE([_PA_C_EXTENSION])
 pa_cv_c_noreturn=no
 for pa_noreturn_extension_try in "$pa_cv_c_extension" ""
 do
 AS_IF([test "x$pa_noreturn_extension_try" != xno],
 [for pa_noreturn_try in ["[[$pa_noreturn_extension_try __noreturn__]]" \
  "$pa_noreturn_extension_try _Noreturn" \
  "__attribute__(($pa_noreturn_extension_try __noreturn__))" \
  "$pa_noreturn_extension_try __declspec(noreturn)"]
 do
  AS_IF([test "x$pa_cv_c_noreturn" = xno],
        [AC_COMPILE_IFELSE([AC_LANG_SOURCE([
AC_INCLUDES_DEFAULT
#define noreturn $pa_noreturn_try
noreturn void here(void);
noreturn void there(void);
noreturn void here(void)
{
	while (1)
	      printf("Loop forever\n");
}
])],[pa_cv_c_noreturn=`pa_fn_trim_whitespace "$pa_noreturn_try"`])])
 done
 ])
 done])])

AC_DEFUN([PA_C_NORETURN],
[AC_REQUIRE([_PA_C_NORETURN])
 AC_REQUIRE([PA_C_EXTENSION])
 AS_IF([test "x$pa_cv_c_noreturn" = xno],
       [pa_c_noreturn=""], [pa_c_noreturn="$pa_cv_c_noreturn"])
 AC_DEFINE_UNQUOTED([noreturn], [$pa_c_noreturn],
       [Define to the best way to declare noreturn functions on this compiler])
])
