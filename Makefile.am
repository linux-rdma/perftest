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

ACLOCAL_AMFLAGS= -I m4
AUTOMAKE_OPTIONS= subdir-objects

noinst_LIBRARIES = libperftest.a
libperftest_a_SOURCES = src/get_clock.c src/perftest_communication.c src/perftest_parameters.c src/perftest_resources.c
noinst_HEADERS = src/get_clock.h src/perftest_communication.h src/perftest_parameters.h src/perftest_resources.h

bin_PROGRAMS = ib_send_bw ib_send_lat ib_write_lat ib_write_bw ib_read_lat ib_read_bw ib_atomic_lat ib_atomic_bw

if HAVE_RAW_ETH
libperftest_a_SOURCES += src/raw_ethernet_resources.c
noinst_HEADERS += src/raw_ethernet_resources.h
bin_PROGRAMS += raw_ethernet_bw raw_ethernet_lat
else
libperftest_a_SOURCES +=
noinst_HEADERS +=
bin_PROGRAMS +=
endif

ib_send_bw_SOURCES = src/send_bw.c src/multicast_resources.c src/multicast_resources.h
ib_send_bw_LDADD = libperftest.a $(LIBUMAD) $(LIBMATH)

ib_send_lat_SOURCES = src/send_lat.c src/multicast_resources.c src/multicast_resources.h
ib_send_lat_LDADD = libperftest.a $(LIBUMAD) $(LIBMATH)

ib_write_lat_SOURCES = src/write_lat.c
ib_write_lat_LDADD = libperftest.a $(LIBMATH) 

ib_write_bw_SOURCES = src/write_bw.c
ib_write_bw_LDADD = libperftest.a $(LIBMATH)

ib_read_lat_SOURCES = src/read_lat.c
ib_read_lat_LDADD = libperftest.a $(LIBMATH)

ib_read_bw_SOURCES = src/read_bw.c
ib_read_bw_LDADD = libperftest.a $(LIBMATH)

ib_atomic_lat_SOURCES = src/atomic_lat.c
ib_atomic_lat_LDADD = libperftest.a $(LIBMATH)

ib_atomic_bw_SOURCES = src/atomic_bw.c
ib_atomic_bw_LDADD = libperftest.a $(LIBMATH)

if HAVE_RAW_ETH
raw_ethernet_bw_SOURCES = src/raw_ethernet_send_bw.c
raw_ethernet_bw_LDADD = libperftest.a $(LIBMATH)

raw_ethernet_lat_SOURCES = src/raw_ethernet_send_lat.c
raw_ethernet_lat_LDADD = libperftest.a $(LIBMATH)

else 
raw_ethernet_bw_SOURCES =
raw_ethernet_bw_LDADD =

raw_ethernet_lat_SOURCES =
raw_ethernet_lat_LDADD =

endif

