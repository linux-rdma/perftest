#
# Copyright (C) Mellanox Technologies Ltd. 2001-2011.  ALL RIGHTS RESERVED.
# This software product is a proprietary product of Mellanox Technologies Ltd.
# (the "Company") and all right, title, and interest and to the software product,
# including all associated intellectual property rights, are and shall
# remain exclusively with the Company.
#
# This software product is governed by the End User License Agreement
# provided with the software product.
# $COPYRIGHT$
# $HEADER$
dnl Process this file with autoconf to produce a configure script.

AC_INIT([perftest],[4.6],[linux-rdma@vger.kernel.org])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_SUBST(MAJOR_VERSION)
AC_SUBST(MINOR_VERSION)

AC_PROG_CC
AC_PROG_CXX
AM_PROG_AS
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_HEADER_STDC
AC_CHECK_HEADERS([infiniband/verbs.h],,[AC_MSG_ERROR([ibverbs header files not found])])
AC_CHECK_LIB([ibverbs], [ibv_get_device_list], [], [AC_MSG_ERROR([libibverbs not found])])
AC_CHECK_LIB([rdmacm], [rdma_create_event_channel], [], AC_MSG_ERROR([librdmacm-devel not found]))
AC_CHECK_LIB([ibumad], [umad_init], [LIBUMAD=-libumad], AC_MSG_ERROR([libibumad not found]))
AC_CHECK_LIB([m], [log], [LIBMATH=-lm], AC_MSG_ERROR([libm not found]))
AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_flow *t = ibv_create_flow(NULL,NULL);],[HAVE_RAW_ETH=yes], [HAVE_RAW_ETH=no])

AM_CONDITIONAL([HAVE_RAW_ETH],[test "x$HAVE_RAW_ETH" = "xyes"])
if test $HAVE_RAW_ETH = yes; then
	AC_DEFINE([HAVE_RAW_ETH], [1], [Enable RAW_ETH_TEST])
fi

AC_SUBST([LIBUMAD])
AC_SUBST([LIBMATH])
CFLAGS="-g -Wall -D_GNU_SOURCE -O2"
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
