dnl --------------------------------------------------------------------------
dnl PA_C_C99_CHECK
dnl
dnl  Check for a C99 minimum compiler level support
dnl --------------------------------------------------------------------------
AC_DEFUN_ONCE([_PA_C_C99],
[AC_CACHE_CHECK([if $CC supports C99],
 [pa_cv_c_c99],
 [AC_REQUIRE([AC_PROG_CC])
  AC_BEFORE([AC_USE_SYSTEM_EXTENSIONS], [$0])
  pa_cv_c_c99=no
AC_LINK_IFELSE([AC_LANG_SOURCE([[
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

const char print_format[] = "%"PRIuMAX"%"PRId16;

struct c99_flex_array {
	unsigned long long llvalue;
        int value;
        char flex@<:@@:>@;
};

static inline bool
c99_supported(const uint16_t values[static 1], uintmax_t *restrict result)
{
        *result = strtoumax("0", NULL, 10);
        return values[0] == 0;
}

int
main(void)
{
        uint16_t values[] = { [0] = 0 };
        uintmax_t result;

        return !c99_supported(values, &result);
}
]])],[pa_cv_c_c99=yes])])])

AC_DEFUN([PA_C_C99_CHECK],
[AC_REQUIRE([_PA_C_C99])
AS_IF([test x$pa_cv_c_c99 != xyes],
[AC_MSG_FAILURE([this package requires a C99 compiler and library])])])
