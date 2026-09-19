dnl --------------------------------------------------------------------------
dnl PA_C_FLEXMEMBER
dnl
dnl Define FLEX to [], [0], or [1] depending on which works...
dnl --------------------------------------------------------------------------
AC_DEFUN([PA_C_FLEXMEMBER],
[AC_CACHE_CHECK([for the syntax $CC uses for structure flex members],
 [pa_cv_flexmember],
 [pa_cv_flexmember=none
 for pa_try_flexmember in ["[]" "[0]" "[1]"]
 do
  AS_IF([test x$pa_cv_flexmember = xnone],
        [AC_COMPILE_IFELSE([AC_LANG_SOURCE([
struct flex {
       int x;
       int y $pa_try_flexmember;
};

int flextest(struct flex *a);
int flextest(struct flex *a)
{
	return a->x + a->y @<:@ 2 @:>@;
}
])],[pa_cv_flexmember="$pa_try_flexmember"])])
 done
  ])
AS_IF([test $pa_cv_flexmember = xnone],
      [AC_MSG_ERROR([No flexible member syntax found.])],
      [pa_flexmember=`printf '%s\n' "$pa_cv_flexmember" | tr -d '[[]]'`
       AC_DEFINE_UNQUOTED([FLEX], [$pa_flexmember],
       [Define to the syntax to use for flexible end of structure array members.])])
])
