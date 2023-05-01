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
dnl Process this file with autoconf to produce a configure script.

AC_INIT([perftest],[6.15],[linux-rdma@vger.kernel.org])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_SUBST(MAJOR_VERSION)
AC_SUBST(MINOR_VERSION)

AC_ARG_VAR(CUDA_H_PATH, help-string)
AC_ARG_VAR(RANLIB, ranlib tool)

AC_ARG_ENABLE([ibv_wr_api],
	[AS_HELP_STRING([--disable-ibv_wr_api],
	[Disable new post send API])])

AS_IF([test "x$enable_ibv_wr_api" = "xno"],
      [USE_IBV_WR_API=no],
        [USE_IBV_WR_API=yes])

AC_PREFIX_DEFAULT("/usr")

AC_PROG_CC
AC_PROG_CXX
AM_PROG_AS
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_PROG_RANLIB
AC_HEADER_STDC
AC_CHECK_HEADERS([infiniband/verbs.h],,[AC_MSG_ERROR([ibverbs header files not found])])
AC_CHECK_HEADERS([sys/random.h],,)
AC_CHECK_LIB([ibverbs], [ibv_get_device_list], [], [AC_MSG_ERROR([libibverbs not found])])
AC_CHECK_LIB([rdmacm], [rdma_create_event_channel], [], AC_MSG_ERROR([librdmacm-devel not found]))
AC_CHECK_LIB([ibumad], [umad_init], [LIBUMAD=-libumad], AC_MSG_ERROR([libibumad not found]))
AC_CHECK_LIB([m], [log], [LIBMATH=-lm], AC_MSG_ERROR([libm not found]))

AC_CHECK_LIB([ibverbs], [ibv_reg_dmabuf_mr], [HAVE_REG_DMABUF_MR=yes], [HAVE_REG_DMABUF_MR=no])
if test $HAVE_REG_DMABUF_MR = yes; then
	AC_DEFINE([HAVE_REG_DMABUF_MR], [1], [Enable HAVE_REG_DMABUF_MR])
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_flow *t = ibv_create_flow(NULL,NULL);],[HAVE_RAW_ETH_REG=yes], [HAVE_RAW_ETH_REG=no])

AM_CONDITIONAL([HAVE_RAW_ETH_REG],[test "x$HAVE_RAW_ETH_REG" = "xyes"])
if test $HAVE_RAW_ETH_REG = yes; then
	AC_DEFINE([HAVE_RAW_ETH_REG], [1], [Enable RAW_ETH_TEST_REG])
fi

AM_CONDITIONAL([HAVE_RAW_ETH],true)
if [test $HAVE_RAW_ETH_REG = yes]; then
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
	LIBMLX5=-lmlx5
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
        [char *c = IBV_LINK_LAYER_SCIF;],[HAVE_SCIF=yes], [HAVE_SCIF=no])

AM_CONDITIONAL([HAVE_SCIF],[test "x$HAVE_SCIF" = "xyes"])
if test $HAVE_SCIF = yes; then
        AC_DEFINE([HAVE_SCIF], [1], [Enable SCIF link Layer])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;],[HAVE_IBV_WR_API=yes],[HAVE_IBV_WR_API=no])
AM_CONDITIONAL([HAVE_IBV_WR_API],[test "x$HAVE_IBV_WR_API" = "xyes"])
# TODO: Add appropriate check. Currently we consider that if rdma-core
# has new post send, it also has ibv_query_gid_type
if [test $HAVE_IBV_WR_API = yes]; then
	AC_DEFINE([HAVE_GID_TYPE], [1], [Have a way to check gid type])
fi
if [test $HAVE_IBV_WR_API = yes] && [test $USE_IBV_WR_API = yes]; then
        AC_DEFINE([HAVE_IBV_WR_API], [1], [Have new post send API support])
else
	HAVE_IBV_WR_API=no
fi

AC_TRY_LINK([
#include <infiniband/mlx5dv.h>],
        [int x = MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;],[HAVE_MLX5DV=yes], [HAVE_MLX5DV=no])
AM_CONDITIONAL([HAVE_MLX5DV],[test "x$HAVE_MLX5DV" = "xyes"])

AC_TRY_LINK([
#include <infiniband/mlx5dv.h>],
        [int x = __devx_dw_off(32);],[HAVE_MLX5_DEVX=yes], [HAVE_MLX5_DEVX=no])
AM_CONDITIONAL([HAVE_MLX5_DEVX],[test "x$HAVE_MLX5_DEVX" = "xyes"])
if [test $HAVE_MLX5_DEVX = yes] && [test $HAVE_MLX5DV = yes];  then
            AC_DEFINE([HAVE_MLX5_DEVX], [1], [Have MLX5 DEVX support])
fi

AC_TRY_LINK([
#include <infiniband/mlx5dv.h>],
        [int x = MLX5DV_QP_INIT_ATTR_MASK_DCI_STREAMS;],[HAVE_DCS=yes], [HAVE_DCS=no])
AM_CONDITIONAL([HAVE_DCS],[test "x$HAVE_DCS" = "xyes"])
if [test $HAVE_DCS = yes] && [test $HAVE_MLX5DV = yes]; then
        AC_DEFINE([HAVE_DCS], [1], [Have DCS support])
fi

AC_TRY_LINK([
#include <infiniband/mlx5dv.h>],
        [int x = MLX5DV_CRYPTO_STANDARD_AES_XTS;],[HAVE_AES_XTS=yes], [HAVE_AES_XTS=no])
AM_CONDITIONAL([HAVE_AES_XTS],[test "x$HAVE_AES_XTS" = "xyes"])
if [test $HAVE_AES_XTS = yes] && [test $HAVE_MLX5DV = yes];  then
            AC_DEFINE([HAVE_AES_XTS], [1], [Have AES XTS support])
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
        [int c = IBV_GID_TYPE_ROCE_V1;],[HAVE_GID_TYPE_DECLARED=yes], [HAVE_GID_TYPE_DECLARED=no])
AM_CONDITIONAL([HAVE_GID_TYPE_DECLARED],[test "x$HAVE_GID_TYPE_DECLARED" = "xyes"])
if [test $HAVE_GID_TYPE_DECLARED = yes]; then
        AC_DEFINE([HAVE_GID_TYPE_DECLARED], [1], [API GID compatibility])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_FLOW_SPEC_IPV6;],[HAVE_IPV6=yes], [HAVE_IPV6=no])
AM_CONDITIONAL([HAVE_IPV6],[test "x$HAVE_IPV6" = "xyes"])
if [test $HAVE_IPV6 = yes]; then
        AC_DEFINE([HAVE_IPV6], [1], [Enable IPv6 Flow Specification])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_FLOW_SPEC_IPV4_EXT;],[HAVE_IPV4_EXT=yes], [HAVE_IPV4_EXT=no])
AM_CONDITIONAL([HAVE_IPV4_EXT],[test "x$HAVE_IPV4_EXT" = "xyes"])
if [test $HAVE_IPV4_EXT = yes]; then
        AC_DEFINE([HAVE_IPV4_EXT], [1], [Enable IPv4 Extended Flow Specification])
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_FLOW_ATTR_SNIFFER;],[HAVE_SNIFFER=yes], [HAVE_SNIFFER=no])
AM_CONDITIONAL([HAVE_SNIFFER],[test "x$HAVE_SNIFFER" = "xyes"])
if [test $HAVE_SNIFFER = yes]; then
        AC_DEFINE([HAVE_SNIFFER], [1], [Enable Sniffer Flow Specification])
fi

if [test $IS_FREEBSD = no]; then
	AC_CHECK_HEADERS([pci/pci.h],,[AC_MSG_ERROR([pciutils header files not found, consider installing pciutils-devel])])
	AC_CHECK_LIB([pci], [pci_init], [LIBPCI=-lpci], AC_MSG_ERROR([libpci not found]))
	CPU_IS_RO_COMPLIANT=yes
fi

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_ACCESS_RELAXED_ORDERING;],[HAVE_RO=yes], [HAVE_RO=no])
AM_CONDITIONAL([HAVE_RO],[test "x$HAVE_RO" = "xyes"])
if [test $HAVE_RO = yes] && [test "x$CPU_IS_RO_COMPLIANT" = "xyes"]; then
        AC_DEFINE([HAVE_RO], [1], [Enable Relaxed Ordering])
	LIBS=$LIBS" -lpci"
fi

AC_TRY_LINK([#include <infiniband/verbs.h>],
	[struct ibv_device_attr_ex *attr_ex; int x = attr_ex->comp_mask;],[HAVE_EX=yes], [HAVE_EX=no])
AM_CONDITIONAL([HAVE_EX],[test "x$HAVE_EX" = "xyes"])
if [test $HAVE_EX = yes]; then
	AC_DEFINE([HAVE_EX], [1], [Have EX support])
fi

AC_ARG_ENABLE([rocm],
              [AS_HELP_STRING([--enable-rocm],
                              [Enable ROCm benchmarks])
              ],
              [],
              [enable_rocm=no])

AC_ARG_WITH([rocm],
            [AS_HELP_STRING([--with-rocm=@<:@ROCm installation path@:>@],
                            [Provide path to ROCm installation])
            ],
            [AS_CASE([$with_rocm],
                     [yes|no], [],
                     [CPPFLAGS="-I$with_rocm/include $CPPFLAGS"
                      LDFLAGS="-L$with_rocm/lib64 -Wl,-rpath=$with_rocm/lib64 -L$with_rocm/lib -Wl,-rpath=$with_rocm/lib -lamdhip64 $LDFLAGS"])
            ])

AS_IF([test "x$enable_rocm" = xyes], [
       AC_DEFINE([__HIP_PLATFORM_HCC__], [1], [Enable ROCm])
       AC_CHECK_HEADERS([hip/hip_runtime_api.h], [],
                        [AC_MSG_ERROR([cannot include hip/hip_runtime_api.h])])
       AC_SEARCH_LIBS([hipFree], [amdhip64], [],
                      [AC_MSG_ERROR([cannot link with -lamdhip64])])
       AC_DEFINE([HAVE_ROCM], [1], [Enable ROCm])
       ])

AM_CONDITIONAL([ROCM], [test x$enable_rocm = xyes])

AC_TRY_LINK([
#include <infiniband/verbs.h>],
        [int x = IBV_ACCESS_ON_DEMAND;],[HAVE_EX_ODP=yes], [HAVE_EX_ODP=no])
AM_CONDITIONAL([HAVE_EX_ODP],[test "x$HAVE_EX_ODP" = "xyes"])
if [test $HAVE_EX_ODP = yes] && [test $HAVE_EX = yes]; then
        AC_DEFINE([HAVE_EX_ODP], [1], [Have Extended  ODP support])
fi

if [test "$CUDA_H_PATH" ]; then
	AC_DEFINE([HAVE_CUDA], [1], [Enable CUDA feature])
	AC_DEFINE_UNQUOTED([CUDA_PATH], "$CUDA_H_PATH" , [Enable CUDA feature])
	LIBS=$LIBS" -lcuda"
	AC_CHECK_LIB([cuda], [cuMemGetHandleForAddressRange], [HAVE_CUDA_CUMEMGETHANDLEFORADDRESSRANGE=yes], [HAVE_CUDA_CUMEMGETHANDLEFORADDRESSRANGE=no])
	AC_TRY_LINK([
	#include <$CUDA_H_PATH>],
	[int x = CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD|CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED;],
	[CUDA_DMA_BUF_PARAMETERS_SUPPORT=yes], [CUDA_DMA_BUF_PARAMETERS_SUPPORT=no])
	if [test "x$HAVE_REG_DMABUF_MR" = "xyes"] && [test "x$HAVE_CUDA_CUMEMGETHANDLEFORADDRESSRANGE" = "xyes"] && [test "x$CUDA_DMA_BUF_PARAMETERS_SUPPORT" = "xyes"]; then
		AC_DEFINE([HAVE_CUDA_DMABUF], [1], [Enable CUDA DMABUF feature])
	fi
fi
AM_CONDITIONAL([CUDA_DMA_BUF_PARAMETERS_SUPPORT],[test "x$CUDA_DMA_BUF_PARAMETERS_SUPPORT" = "xyes"])

AM_CONDITIONAL([CUDA], [test "$CUDA_H_PATH"])

AC_ARG_ENABLE([neuron],
              [AS_HELP_STRING([--enable-neuron],
                              [Enable Neuron benchmarks])
              ],
              [],
              [enable_neuron=no])

AC_ARG_WITH([neuron],
            [AS_HELP_STRING([--with-neuron=@<:@NRT installation path@:>@],
                            [Provide path to NRT installation])
            ],
            [AS_CASE([$with_neuron],
                     [yes|no], [],
                     [CPPFLAGS="-I$with_neuron/include $CPPFLAGS"
                      LDFLAGS="-L$with_neuron/lib -Wl,-rpath=$with_neuron/lib $LDFLAGS"])
            ])

AS_IF([test "x$enable_neuron" = xyes], [
       AC_DEFINE([HAVE_NEURON], [1], [Enable Neuron benchmarks])
       AC_CHECK_HEADERS([nrt/nrt.h], [],
                        [AC_MSG_ERROR([could not find nrt.h in include path])])
       AC_SEARCH_LIBS([nrt_tensor_allocate], [nrt], [],
                      [AC_MSG_ERROR([could not find library, nrt])])
       ])

AM_CONDITIONAL([NEURON], [test x$enable_neuron = xyes])

AC_ARG_ENABLE([hl],
	[AS_HELP_STRING([--enable-hl],
		[Enable Habana Labs benchmarks])
	],
	[],
	[enable_hl=no])

AC_ARG_WITH([hl],
	[AS_HELP_STRING([--with-hl=@<:@habanalabs installation prefix@:>@],
		[Provide Habana Labs installation prefix])
	],
	[AS_CASE([$with_hl],
		[yes|no], [],
		[CPPFLAGS="-I$with_hl/include/habanalabs $CPPFLAGS"
		LDFLAGS="-L$with_hl/lib/habanalabs -Wl,-rpath=$with_hl/lib/habanalabs $LDFLAGS"])
	])

AS_IF([test "x$enable_hl" = xyes], [
	AC_DEFINE([HAVE_HL], [1], [Enable Habana Labs benchmarks])
	AS_IF([test "x$HAVE_REG_DMABUF_MR" = "xno"],
		[AC_MSG_ERROR([rdma-core doesn't support dmabuf mr registration])])
	AC_CHECK_HEADERS([misc/habanalabs.h hlthunk.h synapse_api.h], [],
		[AC_MSG_ERROR([could not find hl headers in include path])])
	AC_SEARCH_LIBS([hlthunk_device_memory_export_dmabuf_fd], [hl-thunk], [],
		[AC_MSG_ERROR([could not find library, hl-thunk])])
	AC_SEARCH_LIBS([synDeviceMalloc], [Synapse], [],
		[AC_MSG_ERROR([could not find library, Synapse])])
	])

AM_CONDITIONAL([HABANALABS], [test x$enable_hl = xyes])

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

if [test $HAVE_IBV_WR_API = yes]; then
	AC_CHECK_LIB([efa], [efadv_create_qp_ex], [HAVE_SRD=yes], [HAVE_SRD=no])
	AC_TRY_LINK([#include <infiniband/efadv.h>],
		    [int x = EFADV_DEVICE_ATTR_CAPS_RDMA_READ;], [HAVE_RDMA_READ_SRD=yes], [HAVE_RDMA_READ_SRD=no])
	if [test $HAVE_RDMA_READ_SRD = yes]; then
		AC_DEFINE([HAVE_SRD_WITH_RDMA_READ], [1], [Have SRD with RDMA read support])
	fi
	AC_TRY_LINK([#include <infiniband/efadv.h>],
		    [int x = EFADV_DEVICE_ATTR_CAPS_RDMA_WRITE;], [HAVE_RDMA_WRITE_SRD=yes], [HAVE_RDMA_WRITE_SRD=no])
	if [test $HAVE_RDMA_WRITE_SRD = yes]; then
		AC_DEFINE([HAVE_SRD_WITH_RDMA_WRITE], [1], [Have SRD with RDMA write support])
	fi
else
	AC_CHECK_LIB([efa], [efadv_create_driver_qp], [HAVE_SRD=yes], [HAVE_SRD=no])
fi
AM_CONDITIONAL([HAVE_SRD], [test "x$HAVE_SRD" = "xyes"])
if [test $HAVE_SRD = yes]; then
	AC_DEFINE([HAVE_SRD], [1], [Have SRD support])
	AC_SUBST([LIBEFA], [-lefa])
fi

AC_CHECK_LIB([mlx5], [mlx5dv_create_qp], [HAVE_MLX5DV_LIB=yes LIBMLX5=-lmlx5], [HAVE_MLX5DV_LIB=no])
AM_CONDITIONAL([HAVE_MLX5DV_LIB], [test "x$HAVE_MLX5DV_LIB" = "xyes"])
if [test $HAVE_MLX5DV_LIB = yes] && [test $HAVE_MLX5DV = yes]; then
	AC_DEFINE([HAVE_MLX5DV], [1], [Have Direct Verbs support])
	AC_SUBST([LIBMLX5])
fi

CFLAGS="-g -Wall -D_GNU_SOURCE -O3 $CFLAGS"
LDFLAGS="$LDFLAGS"
LIBS=$LIBS" -lpthread"
AC_SUBST([LIBUMAD])
AC_SUBST([LIBMATH])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
