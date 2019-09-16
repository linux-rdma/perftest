#
# Copyright (C) 2001-2011 Mellanox Technologies Ltd.  All rights reserved.
#
# This software is available to you under a choice of one of two
# licenses.  You may choose to be licensed under the terms of the GNU
# General Public License (GPL) Version 2, available from the file
# COPYING in the main directory of this source tree, or the
# OpenIB.org BSD license below:
#
#     Redistribution and use in source and binary forms, with or
#     without modification, are permitted provided that the following
#     conditions are met:
#
#      - Redistributions of source code must retain the above
#        copyright notice, this list of conditions and the following
#        disclaimer.
#
#      - Redistributions in binary form must reproduce the above
#        copyright notice, this list of conditions and the following
#        disclaimer in the documentation and/or other materials
#        provided with the distribution.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
dnl Process this file with autoconf to produce a configure script.

AC_INIT([perftest],[5.60],[linux-rdma@vger.kernel.org])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_SUBST(MAJOR_VERSION)
AC_SUBST(MINOR_VERSION)

AC_ARG_VAR(CUDA_H_PATH, help-string)
AC_ARG_VAR(RANLIB, ranlib tool)

AC_ARG_ENABLE([verbs_exp],
[AS_HELP_STRING([--disable-verbs_exp],
[Disable Verbs Experimental])],
[], [enable_verbs_exp=yes])

AC_ARG_ENABLE([ibv_wr_api],
[AS_HELP_STRING([--enable-ibv_wr_api],
[Enable new post send API])],
[enable_ibv_wr_api=yes], [enable_ibv_wr_api=no])

AS_IF([test "x$enable_verbs_exp" = "xyes"],
      [USE_VERBS_EXP=yes],
        [USE_VERBS_EXP=no])

AS_IF([test "x$enable_ibv_wr_api" = "xyes"],
      [USE_IBV_WR_API=yes],
        [USE_IBV_WR_API=no])

AC_PREFIX_DEFAULT("/usr")

AC_PROG_CC
AC_PROG_CXX
AM_PROG_AS
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_PROG_RANLIB
AC_HEADER_STDC
AC_CHECK_HEADERS([infiniband/verbs.h],,[AC_MSG_ERROR([ibverbs header files not found])])
AC_CHECK_LIB([ibverbs], [ibv_get_device_list], [], [AC_MSG_ERROR([libibverbs not found])])
AC_CHECK_LIB([rdmacm], [rdma_create_event_channel], [], AC_MSG_ERROR([librdmacm-devel not found]))
AC_CHECK_LIB([ibumad], [umad_init], [LIBUMAD=-libumad], AC_MSG_ERROR([libibumad not found]))
AC_CHECK_LIB([m], [log], [LIBMATH=-lm], AC_MSG_ERROR([libm not found]))

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

AC_TRY_LINK([
#if !defined(__FreeBSD__)
#include <endian.h>
#else
#include <sys/endian.h>
#endif],
	[int x = htobe32(0);],[HAVE_ENDIAN=yes], [HAVE_ENDIAN=no])

AM_CONDITIONAL([HAVE_ENDIAN],[test "x$HAVE_ENDIAN" = "xyes"])
if test $HAVE_ENDIAN = yes; then
	AC_DEFINE([HAVE_ENDIAN], [1], [Enable endian conversion])
fi

AC_TRY_LINK([],
	[#if !defined(__FreeBSD__)
	 #error nope
	 #endif],
	[IS_FREEBSD=yes], [IS_FREEBSD=no])

AM_CONDITIONAL([IS_FREEBSD],[test "x$IS_FREEBSD" = "xyes"])
if test $IS_FREEBSD = yes; then
	AC_DEFINE([IS_FREEBSD], [1], [OS is FreeBSD])
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
#include <infiniband/verbs.h>],
        [int x = IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;],[HAVE_IBV_WR_API=yes],[HAVE_IBV_WR_API=no])
AM_CONDITIONAL([HAVE_IBV_WR_API],[test "x$HAVE_IBV_WR_API" = "xyes"])
if [test $HAVE_IBV_WR_API = yes] && [test $USE_IBV_WR_API = yes]; then
        AC_DEFINE([HAVE_IBV_WR_API], [1], [Have new post send API support])
else
	HAVE_IBV_WR_API=no
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

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_INTF_GLOBAL;],[HAVE_ACCL_VERBS=yes], [HAVE_ACCL_VERBS=no])
AM_CONDITIONAL([HAVE_ACCL_VERBS],[test "x$HAVE_ACCL_VERBS" = "xyes"])
if [test $HAVE_ACCL_VERBS = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_ACCL_VERBS], [1], [Enable Accelerated verbs feature])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_FLOW_SPEC_IPV6;],[HAVE_IPV6=yes], [HAVE_IPV6=no])
AM_CONDITIONAL([HAVE_IPV6],[test "x$HAVE_IPV6" = "xyes"])
if [test $HAVE_IPV6 = yes] && [test $HAVE_VERBS_EXP = no]; then
        AC_DEFINE([HAVE_IPV6], [1], [Enable IPv6 Flow Specification])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_FLOW_SPEC_IPV6_EXT;],[HAVE_IPV6=yes], [HAVE_IPV6=no])
AM_CONDITIONAL([HAVE_IPV6],[test "x$HAVE_IPV6" = "xyes"])
if [test $HAVE_IPV6 = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_IPV6], [1], [Enable IPv6 Flow Specification])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_FLOW_SPEC_IPV4_EXT;],[HAVE_IPV4_EXT=yes], [HAVE_IPV4_EXT=no])
AM_CONDITIONAL([HAVE_IPV4_EXT],[test "x$HAVE_IPV4_EXT" = "xyes"])
if [test $HAVE_IPV4_EXT = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_IPV4_EXT], [1], [Enable IPv4 Extended Flow Specification])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_FLOW_SPEC_IPV4_EXT;],[HAVE_IPV4_EXT=yes], [HAVE_IPV4_EXT=no])
AM_CONDITIONAL([HAVE_IPV4_EXT],[test "x$HAVE_IPV4_EXT" = "xyes"])
if [test $HAVE_IPV4_EXT = yes] && [test $HAVE_VERBS_EXP = no]; then
        AC_DEFINE([HAVE_IPV4_EXT], [1], [Enable IPv4 Extended Flow Specification])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_FLOW_ATTR_SNIFFER;],[HAVE_SNIFFER_EXP=yes], [HAVE_SNIFFER_EXP=no])
AM_CONDITIONAL([HAVE_SNIFFER_EXP],[test "x$HAVE_SNIFFER_EXP" = "xyes"])
if [test $HAVE_SNIFFER_EXP = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_SNIFFER_EXP], [1], [Enable Sniffer Flow Specification])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_FLOW_ATTR_SNIFFER;],[HAVE_SNIFFER=yes], [HAVE_SNIFFER=no])
AM_CONDITIONAL([HAVE_SNIFFER],[test "x$HAVE_SNIFFER" = "xyes"])
if [test $HAVE_SNIFFER = yes] && [test $HAVE_VERBS_EXP = no]; then
        AC_DEFINE([HAVE_SNIFFER], [1], [Enable Sniffer Flow Specification])
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_device_attr_ex *attr_ex; int x = attr_ex->comp_mask;],[HAVE_EX=yes], [HAVE_EX=no])
AM_CONDITIONAL([HAVE_EX],[test "x$HAVE_EX" = "xyes"])
if [test $HAVE_EX = yes]; then
	AC_DEFINE([HAVE_EX], [1], [Have EX support])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_ACCESS_ON_DEMAND;],[HAVE_EX_ODP=yes], [HAVE_EX_ODP=no])
AM_CONDITIONAL([HAVE_EX_ODP],[test "x$HAVE_EX_ODP" = "xyes"])
if [test $HAVE_EX_ODP = yes] && [test $HAVE_EX = yes]; then
        AC_DEFINE([HAVE_EX_ODP], [1], [Have Extended  ODP support])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_ACCESS_ON_DEMAND;],[HAVE_EXP_ODP=yes], [HAVE_EXP_ODP=no])
AM_CONDITIONAL([HAVE_EXP_ODP],[test "x$HAVE_EXP_ODP" = "xyes"])
if [test $HAVE_EXP_ODP = yes] && [test $HAVE_VERBS_EXP = yes]; then
	if  [test $HAVE_EX_ODP = no] ; then
		AC_DEFINE([HAVE_EXP_ODP], [1], [Have Experimental ODP support])
	fi
fi

if [test "$CUDA_H_PATH" ]; then
	AC_DEFINE([HAVE_CUDA], [1], [Enable CUDA feature])
	AC_DEFINE_UNQUOTED([CUDA_PATH], "$CUDA_H_PATH" , [Enable CUDA feature])
	LIBS=$LIBS" -lcuda"
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = IBV_EXP_DEVICE_SCATTER_FCS;],[HAVE_SCATTER_FCS=yes], [HAVE_SCATTER_FCS=no])
AM_CONDITIONAL([HAVE_SCATTER_FCS],[test "x$HAVE_SCATTER_FCS" = "xyes"])
if [test $HAVE_SCATTER_FCS = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_SCATTER_FCS], [1], [Have SCATTER_FCS support])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>],
        [int x = ibv_exp_query_gid_attr(NULL, 1, 0, NULL);],[HAVE_GID_ATTR=yes], [HAVE_GID_ATTR=no])
AM_CONDITIONAL([HAVE_GID_ATTR],[test "x$HAVE_GID_ATTR" = "xyes"])
if [test $HAVE_GID_ATTR = yes] && [test $HAVE_VERBS_EXP = yes]; then
        AC_DEFINE([HAVE_GID_ATTR], [1], [Enable GID Attribute query feature])
fi

AC_TRY_LINK([ #include <infiniband/verbs_exp.h>],
	[struct ibv_exp_qp_attr *attr; int x = attr->rate_limit;],[HAVE_PACKET_PACING_EXP=yes], [HAVE_PACKET_PACING_EXP=no])
AM_CONDITIONAL([HAVE_PACKET_PACING_EXP],[test "x$HAVE_PACKET_PACING_EXP" = "xyes"])
if [test $HAVE_PACKET_PACING_EXP = yes] && [test $HAVE_VERBS_EXP = yes]; then
	AC_DEFINE([HAVE_PACKET_PACING_EXP], [1], [Have PACKET_PACING_EXP support])
fi

AC_TRY_LINK([ #include <infiniband/verbs_exp.h>],
	[struct ibv_exp_qp_attr *attr; struct ibv_exp_burst_info x = attr->burst_info;],[HAVE_PACKET_PACING_EXTENSION_EXP=yes], [HAVE_PACKET_PACING_EXTENSION_EXP=no])
AM_CONDITIONAL([HAVE_PACKET_PACING_EXTENSION_EXP],[test "x$HAVE_PACKET_PACING_EXTENSION_EXP" = "xyes"])
if [test $HAVE_PACKET_PACING_EXTENSION_EXP = yes] && [test $HAVE_VERBS_EXP = yes]; then
	AC_DEFINE([HAVE_PACKET_PACING_EXTENSION_EXP], [1], [Have PACKET_PACING_EXTENSION_EXP support])
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_qp_attr *attr; int x = attr->rate_limit;],[HAVE_PACKET_PACING=yes], [HAVE_PACKET_PACING=no])
AM_CONDITIONAL([HAVE_PACKET_PACING],[test "x$HAVE_PACKET_PACING" = "xyes"])
if [test $HAVE_PACKET_PACING = yes]; then
	AC_DEFINE([HAVE_PACKET_PACING], [1], [Have PACKET_PACING support])
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
	[int x = IBV_OOO_RW_DATA_PLACEMENT;], [HAVE_OOO_ATTR=yes], [HAVE_OOO_ATTR=no])
AM_CONDITIONAL([HAVE_OOO_ATTR], [test "x$HAVE_OOO_ATTR" = "xyes"])
if [test $HAVE_OOO_ATTR = yes]; then
	AC_DEFINE([HAVE_OOO_ATTR], [1], [Have Out of order data placement support])
fi

AC_TRY_LINK([#include <infiniband/verbs_exp.h>],
	[int x = IBV_EXP_OOO_SUPPORT_RW_DATA_PLACEMENT;], [HAVE_EXP_OOO_ATTR=yes], [HAVE_EXP_OOO_ATTR=no])
AM_CONDITIONAL([HAVE_EXP_OOO_ATTR], [test "x$HAVE_EXP_OOO_ATTR" = "xyes"])
if [test $HAVE_EXP_OOO_ATTR = yes]; then
	AC_DEFINE([HAVE_EXP_OOO_ATTR], [1], [Have Experimental Out of order data placement support])
fi

AC_CHECK_LIB([efa], [efadv_create_driver_qp], [HAVE_SRD=yes LIBEFA=-lefa], [HAVE_SRD=no])
AM_CONDITIONAL([HAVE_SRD], [test "x$HAVE_SRD" = "xyes"])
if [test $HAVE_SRD = yes] && [test $HAVE_IBV_WR_API = no]; then
	AC_DEFINE([HAVE_SRD], [1], [Have SRD support])
	AC_SUBST([LIBEFA])
fi

CFLAGS="-g -Wall -D_GNU_SOURCE -O3 $CFLAGS"
LIBS=$LIBS" -lpthread"
AC_SUBST([LIBUMAD])
AC_SUBST([LIBMATH])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
