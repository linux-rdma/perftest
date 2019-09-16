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

ACLOCAL_AMFLAGS= -I m4
AUTOMAKE_OPTIONS= subdir-objects

noinst_LIBRARIES = libperftest.a
libperftest_a_SOURCES = src/get_clock.c src/perftest_communication.c src/perftest_parameters.c src/perftest_resources.c src/perftest_counters.c
noinst_HEADERS = src/get_clock.h src/perftest_communication.h src/perftest_parameters.h src/perftest_resources.h src/perftest_counters.h

bin_PROGRAMS = ib_send_bw ib_send_lat ib_write_lat ib_write_bw ib_read_lat ib_read_bw ib_atomic_lat ib_atomic_bw
bin_SCRIPTS = run_perftest_loopback run_perftest_multi_devices

if HAVE_RAW_ETH
libperftest_a_SOURCES += src/raw_ethernet_resources.c
noinst_HEADERS += src/raw_ethernet_resources.h
bin_PROGRAMS += raw_ethernet_bw raw_ethernet_lat raw_ethernet_burst_lat raw_ethernet_fs_rate
else
libperftest_a_SOURCES +=
noinst_HEADERS +=
bin_PROGRAMS +=
endif

if IS_FREEBSD
LIBMLX4= -lmlx4
LIBMLX5= -lmlx5
else
LIBMLX4=
LIBMLX5=
endif

ib_send_bw_SOURCES = src/send_bw.c src/multicast_resources.c src/multicast_resources.h
ib_send_bw_LDADD = libperftest.a $(LIBUMAD) $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

ib_send_lat_SOURCES = src/send_lat.c src/multicast_resources.c src/multicast_resources.h
ib_send_lat_LDADD = libperftest.a $(LIBUMAD) $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

ib_write_lat_SOURCES = src/write_lat.c
ib_write_lat_LDADD = libperftest.a $(LIBMATH)  $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

ib_write_bw_SOURCES = src/write_bw.c
ib_write_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

ib_read_lat_SOURCES = src/read_lat.c
ib_read_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

ib_read_bw_SOURCES = src/read_bw.c
ib_read_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

ib_atomic_lat_SOURCES = src/atomic_lat.c
ib_atomic_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

ib_atomic_bw_SOURCES = src/atomic_bw.c
ib_atomic_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

if HAVE_RAW_ETH
raw_ethernet_bw_SOURCES = src/raw_ethernet_send_bw.c
raw_ethernet_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

raw_ethernet_lat_SOURCES = src/raw_ethernet_send_lat.c
raw_ethernet_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

raw_ethernet_burst_lat_SOURCES = src/raw_ethernet_send_burst_lat.c
raw_ethernet_burst_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

raw_ethernet_fs_rate_SOURCES = src/raw_ethernet_fs_rate.c
raw_ethernet_fs_rate_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA)

else
raw_ethernet_bw_SOURCES =
raw_ethernet_bw_LDADD =

raw_ethernet_lat_SOURCES =
raw_ethernet_lat_LDADD =

endif

