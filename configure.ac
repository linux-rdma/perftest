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

AC_INIT([perftest],[5.33],[linux-rdma@vger.kernel.org])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_SUBST(MAJOR_VERSION)
AC_SUBST(MINOR_VERSION)

AC_ARG_VAR(CUDA_H_PATH, help-string)

AC_ARG_ENABLE([verbs_exp],
[AS_HELP_STRING([--disable-verbs_exp],
[Disable Verbs Experimental])],
[], [enable_verbs_exp=yes])

AS_IF([test "x$enable_verbs_exp" = "xyes"],
      [USE_VERBS_EXP=yes],
        [USE_VERBS_EXP=no])

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
AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_ACCESS_ON_DEMAND;],[HAVE_ODP=yes], [HAVE_ODP=no])
AM_CONDITIONAL([HAVE_ODP],[test "x$HAVE_ODP" = "xyes"])
if [test $HAVE_ODP = yes] && [test $HAVE_ODP = yes]; then
        AC_DEFINE([HAVE_ODP], [1], [Have ODP support])
fi



AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_exp_flow *t = ibv_exp_create_flow(NULL,NULL);],[HAVE_RAW_ETH_EXP=yes], [HAVE_RAW_ETH_EXP=no])

AM_CONDITIONAL([HAVE_RAW_ETH_EXP],[test "x$HAVE_RAW_ETH_EXP" = "xyes"])
if test $HAVE_RAW_ETH_EXP = yes; then
	AC_DEFINE([HAVE_RAW_ETH_EXP], [1], [Enable RAW_ETH_TEST_EXP])
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_flow *t = ibv_create_flow(NULL,NULL);],[HAVE_RAW_ETH_REG=yes], [HAVE_RAW_ETH_REG=no])

AM_CONDITIONAL([HAVE_RAW_ETH_REG],[test "x$HAVE_RAW_ETH_REG" = "xyes"])
if test $HAVE_RAW_ETH_REG = yes; then
	AC_DEFINE([HAVE_RAW_ETH_REG], [1], [Enable RAW_ETH_TEST_REG])
fi

AM_CONDITIONAL([HAVE_RAW_ETH],true)
if [test $HAVE_RAW_ETH_REG = yes] || [test $HAVE_RAW_ETH_EXP = yes]; then
	AC_DEFINE([HAVE_RAW_ETH], [1], [Enable RAW_ETH_TEST])
else
	AM_CONDITIONAL([HAVE_RAW_ETH],false)
fi


AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_xrcd *t = ibv_open_xrcd(NULL,NULL);],[HAVE_XRCD=yes], [HAVE_XRCD=no])

AM_CONDITIONAL([HAVE_XRCD],[test "x$HAVE_XRCD" = "xyes"])
if test $HAVE_XRCD = yes; then
	AC_DEFINE([HAVE_XRCD], [1], [Enable XRCD feature])
fi

AC_TRY_LINK([#include <endian.h>],
	[int x = htobe32(0);],[HAVE_ENDIAN=yes], [HAVE_ENDIAN=no])

AM_CONDITIONAL([HAVE_ENDIAN],[test "x$HAVE_ENDIAN" = "xyes"])
if test $HAVE_ENDIAN = yes; then
	AC_DEFINE([HAVE_ENDIAN], [1], [Enable endian conversion])
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
        [char *c = IBV_LINK_LAYER_SCIF;],[HAVE_SCIF=yes], [HAVE_SCIF=no])

AM_CONDITIONAL([HAVE_SCIF],[test "x$HAVE_SCIF" = "xyes"])
if test $HAVE_SCIF = yes; then
        AC_DEFINE([HAVE_SCIF], [1], [Enable SCIF link Layer])
fi

AC_CHECK_HEADERS([infiniband/verbs_exp.h],[HAVE_VERBS_EXP=yes],[HAVE_VERBS_EXP=no],[#include <infiniband/verbs.h>])
AM_CONDITIONAL([HAVE_VERBS_EXP],[test "x$HAVE_VERBS_EXP" = "xyes"])
if [test $HAVE_VERBS_EXP = yes] && [test $USE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_VERBS_EXP], [1], [Have verbs_exp header])
else
	HAVE_VERBS_EXP=no
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_DEVICE_ATTR_EXT_ATOMIC_ARGS;],[HAVE_MASKED_ATOMICS=yes], [HAVE_MASKED_ATOMICS=no])
AM_CONDITIONAL([HAVE_MASKED_ATOMICS],[test "x$HAVE_MASKED_ATOMICS" = "xyes"])
if [test $HAVE_MASKED_ATOMICS = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_MASKED_ATOMICS], [1], [Have MASKED ATOMICS in verbs_exp header])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ;],[HAVE_RSS_EXP=yes], [HAVE_RSS_EXP=no])
AM_CONDITIONAL([HAVE_RSS_EXP],[test "x$HAVE_RSS_EXP" = "xyes"])
if [test $HAVE_RSS_EXP = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_RSS_EXP], [1], [Have RSS in verbs_exp header])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_DCT_CREATE_FLAGS_MASK;],[HAVE_DC=yes], [HAVE_DC=no])
AM_CONDITIONAL([HAVE_DC],[test "x$HAVE_DC" = "xyes"])
if [test $HAVE_DC = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_DC], [1], [Enable DC feature])
fi

if [test "$CUDA_H_PATH" ]; then
	AC_DEFINE([HAVE_CUDA], [1], [Enable CUDA feature])
	AC_DEFINE_UNQUOTED([CUDA_PATH], "$CUDA_H_PATH" , [Enable CUDA feature])
	LIBS=$LIBS" -lcuda"
fi

CFLAGS="-g -Wall -D_GNU_SOURCE -O3"
AC_SUBST([LIBUMAD])
AC_SUBST([LIBMATH])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
