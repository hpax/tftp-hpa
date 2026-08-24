dnl --------------------------------------------------------------------------
dnl PA_DEPEND
dnl
dnl  Add a dependency generation pattern to each langflag that is
dnl  supported. DEPENDFILE should be set in the Makefile to the
dnl  desired pattern for the dependency file name.
dnl --------------------------------------------------------------------------
AC_DEFUN([PA_DEPEND],
 [
dnl This is super ugly: during autoconf this will create a file
dnl literally named ${DEPENDFILE}, but it does get expanded during make.
dnl The DEPENDFILE setting below only affects the output message.
  DEPENDFILE=file.dep
  PA_ADD_LANGFLAGS([-MMD -MF ${DEPENDFILE}],[-MD -MF ${DEPENDFILE}])
  rm -f '${DEPENDFILE}'
 ])
