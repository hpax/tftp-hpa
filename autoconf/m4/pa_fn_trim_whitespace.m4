dnl --------------------------------------------------------------------------
dnl PA_FN_TRIM_WHITESPACE
dnl
dnl  A shell function to trim whilespace from a list of arguments
dnl --------------------------------------------------------------------------
AC_DEFUN_ONCE([PA_FN_TRIM_WHITESPACE],
[pa_fn_trim_whitespace ()
{
  printf "%s\n" "[$]*" | sed '
s/	/ /g
s/^  *//
s/  *$//
s/   */ /g
'
}])
