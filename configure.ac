AC_PREREQ(2.53)
AC_INIT([xresponse], 0.3.1, [ross@o-hand.com])
AC_CONFIG_SRCDIR([xresponse.c])

AM_INIT_AUTOMAKE()

AM_CONFIG_HEADER([config.h])

# Checks for programs.
AC_PROG_CC

#PKG_CHECK_MODULES(X11, x11, [have_libx11pc="yes"], [have_libx11pc="no"])

# Very lazy check, possibly do old way aswell, but damage will be needed 
# whatever so likely will need autoconfed ( fd.o ) xlibs.
PKG_CHECK_MODULES(XLIBS, x11 xext xtst xdamage)

AC_SUBST(XLIBS_CFLAGS)
AC_SUBST(XLIBS_LIBS)

if test "x$GCC" = "xyes"; then
        GCC_FLAGS="-g -Wall"
fi

AC_SUBST(GCC_FLAGS)

# Checks for header files.
AC_OUTPUT([Makefile])
