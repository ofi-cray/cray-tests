AC_PREREQ([2.63])
AC_INIT([cray-tests], [0.5])
AC_CONFIG_AUX_DIR(config)
AC_CONFIG_MACRO_DIR(config)
AC_CONFIG_HEADERS(config.h)
AM_INIT_AUTOMAKE([1.11 dist-bzip2 foreign -Wall -Werror subdir-objects])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_CANONICAL_HOST

case $host_os in
*linux*)
	;;
*)
	AC_MSG_ERROR([cray-tests only builds on Linux])
	;;
esac

m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

AM_PROG_LIBTOOL

AC_PROG_CC
AM_PROG_CC_C_O

LT_INIT

FI_PKG_CHECK_MODULES([DRC], [cray-drc],
		     [CPPFLAGS="$DRC_CFLAGS $CPPFLAGS"
		      LDFLAGS="$DRC_LIBS $LDFLAGS"
		      drc_happy=1],
		     [drc_happy=0])

AC_DEFINE([HAVE_DRC], [$drc_happy], ["Does the project have access to DRC?"])
AM_CONDITIONAL([HAVE_DRC], [test $drc_happy -eq 1])

AC_SUBST([HAVE_DRC])

AC_ARG_WITH([libfabric],
            AC_HELP_STRING([--with-libfabric], [Use non-default libfabric location - default NO]),
            [AS_IF([test -d $withval/lib64], [fab_libdir="lib64"], [fab_libdir="lib"])
             CPPFLAGS="-I $withval/include $CPPFLAGS"
             LDFLAGS="-L$withval/$fab_libdir $LDFLAGS"],
            [])

dnl I think we'd rather just search for the pmi module
AC_ARG_WITH([pmi],
            AC_HELP_STRING([--with-pmi], [Specify PMI location - default NO]),
            [AS_IF([test -d $withval/lib64], [pmi_libdir="lib64"], [pmi_libdir="lib"])
             CPPFLAGS="-I $withval/include $CPPFLAGS"
             CPPFLAGS="-I $withval $CPPFLAGS"
             LDFLAGS="-L$withval/$pmi_libdir $LDFLAGS"
             AM_CONDITIONAL([HAVE_PMI], [true])],
            [AM_CONDITIONAL([HAVE_PMI], [false])])

AC_CHECK_LIB([pmi], [PMI_Allgather],
	     AM_CONDITIONAL([HAVE_CRAY_PMI], [true]),
	     AM_CONDITIONAL([HAVE_CRAY_PMI], [false]))

AC_ARG_WITH([profiler],
            AC_HELP_STRING([--with-profiler], [Specify libprofiler location - default NO]),
            [AS_IF([test -d $withval/lib64], [profiler_libdir="lib64"], [profiler_libdir="lib"])
             CPPFLAGS="-I $withval/include $CPPFLAGS"
             LDFLAGS="-L$withval/$profiler_libdir $LDFLAGS"
             AM_CONDITIONAL([HAVE_PROFILE], [true])],
            [AM_CONDITIONAL([HAVE_PROFILE], [false])])

AC_CHECK_LIB([profiler], [ProfilerStart],
	     AM_CONDITIONAL([HAVE_CPUPROFILE], [true]),
	     AM_CONDITIONAL([HAVE_CPUPROFILE], [false]))


dnl Checks for libraries
AC_CHECK_LIB([fabric], fi_getinfo, [],
    AC_MSG_ERROR([fi_getinfo() not found.  cray-tests requires libfabric.]))

dnl Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADER([rdma/fabric.h], [],
    [AC_MSG_ERROR([<rdma/fabric.h> not found.  cray-tests requires libfabric.])])

AC_MSG_CHECKING([for fi_trywait support])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <rdma/fi_eq.h>]],
	       [[fi_trywait(NULL, NULL, 0);]])],
	       [AC_MSG_RESULT([yes])],
	       [AC_MSG_RESULT([no])
	        AC_MSG_ERROR([cray-tests requires fi_trywait support. Cannot continue])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
