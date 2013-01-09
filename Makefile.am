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
#

noinst_LTLIBRARIES = libperftest.la
bin_PROGRAMS     = ib_send_bw ib_send_lat ib_write_lat ib_write_bw ib_read_lat ib_read_bw ib_atomic_lat ib_atomic_bw raw_ethernet_bw
ACLOCAL_AMFLAGS=-I m4

INCLUDES := -I$(top_srcdir) -I$(top_srcdir)/..

ib_send_bw_SOURCES = src/send_bw.c
ib_send_bw_LDADD = libperftest.la
ib_send_bw_CFLAGS = $(AM_CFLAGS)

ib_send_lat_SOURCES = src/send_lat.c
ib_send_lat_LDADD = libperftest.la
ib_send_lat_CFLAGS = $(AM_CFLAGS)

ib_write_lat_SOURCES = src/write_lat.c
ib_write_lat_LDADD = libperftest.la
ib_write_lat_CFLAGS = $(AM_CFLAGS)

ib_write_bw_SOURCES = src/write_bw.c
ib_write_bw_LDADD = libperftest.la
ib_write_bw_CFLAGS = $(AM_CFLAGS)

ib_read_lat_SOURCES = src/read_lat.c
ib_read_lat_LDADD = libperftest.la
ib_read_lat_CFLAGS = $(AM_CFLAGS)

ib_read_bw_SOURCES = src/read_bw.c
ib_read_bw_LDADD = libperftest.la
ib_read_bw_CFLAGS = $(AM_CFLAGS)

ib_atomic_lat_SOURCES = src/atomic_lat.c
ib_atomic_lat_LDADD = libperftest.la
ib_atomic_lat_CFLAGS = $(AM_CFLAGS)

ib_atomic_bw_SOURCES = src/atomic_bw.c
ib_atomic_bw_LDADD = libperftest.la
ib_atomic_bw_CFLAGS = $(AM_CFLAGS)

raw_ethernet_bw_SOURCES = src/raw_ethernet_send_bw.c
raw_ethernet_bw_LDADD = libperftest.la 
raw_ethernet_bw_CFLAGS = $(AM_CFLAGS)
	
noinst_HEADERS = \
				 src/get_clock.h \
				 src/get_clock_win.h \
				 src/multicast_resources.h \
				 src/perftest_communication.h \
				 src/perftest_parameters.h \
				 src/perftest_resources.h
	
libperftest_la_SOURCES = \
						 src/atomic_bw.c \
						 src/atomic_lat.c \
						 src/clock_test.c \
						 src/get_clock.c \
						 src/multicast_resources.c \
						 src/perftest_communication.c \
						 src/perftest_parameters.c \
						 src/perftest_resources.c \
						 src/read_bw.c \
						 src/read_lat.c \
						 src/send_bw.c \
						 src/send_lat.c \
						 src/write_bw.c \
						 src/raw_ethernet_send_bw.c \
						 src/write_lat.c 
