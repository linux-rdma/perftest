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
libperftest_a_SOURCES = src/get_clock.c src/perftest_communication.c src/perftest_negotiation.c src/perftest_parameters.c src/perftest_resources.c src/perftest_counters.c src/host_memory.c src/host_validation.c src/mmap_memory.c src/numa_loader.c src/mpi_loader.c src/perftest_cluster.c
noinst_HEADERS = src/get_clock.h src/perftest_communication.h src/perftest_negotiation.h src/perftest_parameters.h src/perftest_resources.h src/perftest_counters.h src/memory.h src/host_memory.h src/host_validation.h src/mmap_memory.h src/cuda_memory.h src/rocm_memory.h src/neuron_memory.h src/hl_memory.h src/mlu_memory.h src/cuda_validation.h src/validation_common.h src/dm_memory.h src/musa_memory.h src/numa_loader.h src/mpi_loader.h src/perftest_cluster.h

AM_CFLAGS =

# SIMD flags for host validation only (detected by configure)
src/host_validation.$(OBJEXT): CFLAGS += @SIMD_CFLAGS@

if CUDA
libperftest_a_SOURCES += src/cuda_memory.c src/cuda_loader.h src/cuda_loader.c
endif

if MUSA
libperftest_a_SOURCES += src/musa_memory.c src/musa_loader.h src/musa_loader.c
endif

if ROCM
libperftest_a_SOURCES += src/rocm_memory.c
endif

if NEURON
libperftest_a_SOURCES += src/neuron_memory.c
endif

if MLU
libperftest_a_SOURCES += src/mlu_memory.c
endif

if HABANALABS
libperftest_a_SOURCES += src/hl_memory.c
endif

if OPENCL
libperftest_a_SOURCES += src/opencl_memory.c
endif

if IB_DM_DMABUF
libperftest_a_SOURCES += src/dm_memory.c
endif

bin_PROGRAMS = ib_send_bw ib_send_lat ib_write_lat ib_write_bw ib_read_lat ib_read_bw ib_atomic_lat ib_atomic_bw perftest_cluster_worker
bin_SCRIPTS = run_perftest_loopback run_perftest_multi_devices perftest_cluster perftest_nic_gpu_discover

# Cluster mode scripts. perftest_cluster substitutes $(libexecdir) at make time;
# perftest_nic_gpu_discover is an executable copy of nic_gpu_discovery.py for remote SSH use.
do_subst = sed -e 's,[@]libexecdir[@],$(libexecdir),g'

perftest_cluster: scripts/perftest_cluster.in
	$(do_subst) < $< > $@
	chmod +x $@

perftest_nic_gpu_discover: cluster_mode/nic_gpu_discovery.py
	cp $< $@
	chmod +x $@

CLEANFILES = perftest_cluster perftest_nic_gpu_discover

EXTRA_DIST = scripts/perftest_cluster.in cluster_mode/nic_gpu_discovery.py

# Install cluster_mode/*.py to $(libexecdir)/perftest/cluster_mode/.
# These are runtime-loaded by the 'perftest_cluster' wrapper via sys.path injection.
clustermodedir = $(libexecdir)/perftest/cluster_mode
dist_clustermode_DATA = \
	cluster_mode/__init__.py \
	cluster_mode/__main__.py \
	cluster_mode/config_model.py \
	cluster_mode/orchestrator.py \
	cluster_mode/discovery.py \
	cluster_mode/json_report.py \
	cluster_mode/mpi_command.py \
	cluster_mode/metrics.py \
	cluster_mode/perftest_output.py \
	cluster_mode/traffic_patterns.py \
	cluster_mode/config_parser.py \
	cluster_mode/remote.py \
	cluster_mode/rendering.py \
	cluster_mode/result_schema.py \
	cluster_mode/mpi_env.py

# Non-source man pages:
man1_MANS = \
	$(top_builddir)/man/ib_write_bw.1 \
	$(top_builddir)/man/ib_read_bw.1 \
	$(top_builddir)/man/ib_send_bw.1 \
	$(top_builddir)/man/ib_atomic_bw.1 \
	$(top_builddir)/man/ib_write_lat.1 \
	$(top_builddir)/man/ib_read_lat.1 \
	$(top_builddir)/man/ib_send_lat.1 \
	$(top_builddir)/man/ib_atomic_lat.1 \
	$(top_builddir)/man/raw_ethernet_bw.1 \
	$(top_builddir)/man/raw_ethernet_lat.1 \
	$(top_builddir)/man/raw_ethernet_burst_lat.1 \
	$(top_builddir)/man/raw_ethernet_fs_rate.1 \
	#

dist_man_MANS = man/perftest.1

generated_mans = $(man1_MANS)
$(generated_mans): $(top_builddir)/man/%.1: man/perftest.1
	@if echo $(top_srcdir) | grep '^/' > /dev/null ; then \
	  echo ln -s $(top_srcdir)/man/perftest.1 $@ ; \
	  ln -s $(top_srcdir)/man/perftest.1 $@ ; \
	else \
	  echo ln -s ../$(top_srcdir)/man/perftest.1 $@ ; \
	  ln -s ../$(top_srcdir)/man/perftest.1 $@ ; \
	fi

clean-local:
	-rm -f man/[ir]*.1
	-rm -f libperftest_kernels.so src/cuda_kernels.cu.o

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
else
LIBMLX4=
endif

if HAVE_CUDART

SUFFIXES= .cu

# Validation kernels + GPU touch compiled into a separate shared library (plugin)
# This allows perftest to run on systems without CUDA installed
src/cuda_kernels.cu.o: src/cuda_kernels.cu
	$(NVCC) -DHAVE_CONFIG_H $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) -Xcompiler -fPIC -o $@ -c $< $(NVCCFLAGS)

libperftest_kernels.so: src/cuda_kernels.cu.o
	$(NVCC) -shared -o $@ $< -L$(CUDA_LIB_DIR) -lcudart

# Build the kernel plugin alongside the main binary
all-local: libperftest_kernels.so

# Install the kernel plugin
install-exec-local: libperftest_kernels.so
	$(INSTALL) -d $(DESTDIR)$(libdir)
	$(INSTALL) libperftest_kernels.so $(DESTDIR)$(libdir)/

# Clean up kernel plugin
clean-local-cuda:
	-rm -f libperftest_kernels.so src/cuda_kernels.cu.o
endif


ib_send_bw_SOURCES = src/send_bw.c src/multicast_resources.c src/multicast_resources.h
ib_send_bw_LDADD = libperftest.a $(LIBUMAD) $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
ib_send_lat_SOURCES = src/send_lat.c src/multicast_resources.c src/multicast_resources.h
ib_send_lat_LDADD = libperftest.a $(LIBUMAD) $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
ib_write_lat_SOURCES = src/write_lat.c
ib_write_lat_LDADD = libperftest.a $(LIBMATH)  $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
ib_write_bw_SOURCES = src/write_bw.c
ib_write_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
ib_read_lat_SOURCES = src/read_lat.c
ib_read_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
ib_read_bw_SOURCES = src/read_bw.c
ib_read_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
ib_atomic_lat_SOURCES = src/atomic_lat.c
ib_atomic_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
ib_atomic_bw_SOURCES = src/atomic_bw.c
ib_atomic_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)

perftest_cluster_worker_SOURCES = src/perftest_cluster_worker.c
perftest_cluster_worker_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)

if HAVE_RAW_ETH
raw_ethernet_bw_SOURCES = src/raw_ethernet_send_bw.c
raw_ethernet_bw_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
raw_ethernet_lat_SOURCES = src/raw_ethernet_send_lat.c
raw_ethernet_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
raw_ethernet_burst_lat_SOURCES = src/raw_ethernet_send_burst_lat.c
raw_ethernet_burst_lat_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
raw_ethernet_fs_rate_SOURCES = src/raw_ethernet_fs_rate.c
raw_ethernet_fs_rate_LDADD = libperftest.a $(LIBMATH) $(LIBMLX4) $(LIBMLX5) $(LIBEFA) $(LIBHNS)
else
raw_ethernet_bw_SOURCES =
raw_ethernet_bw_LDADD =

raw_ethernet_lat_SOURCES =
raw_ethernet_lat_LDADD =

endif
