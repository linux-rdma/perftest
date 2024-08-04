cmake_minimum_required(VERSION 3.7...3.29 FATAL_ERROR)

project(perftest C)
set(VERSION 6.22)
set(CMAKE_BUILD_TYPE "RelWithDebInfo")

# Options taken from `configure.ac`, enable some of them by default,
# should use `try_compile` in the future for better compatibility.
option(HAVE_DLFCN_H "Enable HAVE_DLFCN_H" ON)
option(HAVE_ENDIAN "Enable HAVE_ENDIAN" ON)
option(HAVE_EX "Enable HAVE_EX" ON)
option(HAVE_EX_ODP "Enable HAVE_EX_ODP" ON)
option(HAVE_GID_TYPE "Enable HAVE_GID_TYPE" ON)
option(HAVE_IBV_WR_API "Enable HAVE_IBV_WR_API" ON)
option(HAVE_IPV4_EXT "Enable HAVE_IPV4_EXT" ON)
option(HAVE_IPV6 "Enable HAVE_IPV6" ON)
option(HAVE_PACKET_PACING "Enable HAVE_PACKET_PACING" ON)
option(HAVE_RAW_ETH "Enable HAVE_RAW_ETH" ON)
option(HAVE_RAW_ETH_REG "Enable HAVE_RAW_ETH_REG" ON)
option(HAVE_RO "Enable HAVE_RO" ON)
option(HAVE_SNIFFER "Enable HAVE_SNIFFER" ON)
option(HAVE_XRCD "Enable HAVE_XRCD" ON)

# Enable CUDA if user specified CUDA header path
if (DEFINED CUDA_H_PATH)
    message("CUDA_H_PATH is set to ${CUDA_H_PATH}")
    set(CUDA_PATH ${CUDA_H_PATH})
    option(HAVE_CUDA "Enable HAVE_CUDA" ON)
    option(HAVE_CUDA_DMABUF "Enable HAVE_CUDA_DMABUF" ON)
else()
    option(HAVE_CUDA "Enable HAVE_CUDA" OFF)
    option(HAVE_CUDA_DMABUF "Enable HAVE_CUDA_DMABUF" OFF)
endif()

# Enable these on demand
option(ENABLE_HL "Enable ENABLE_HL" OFF)
option(ENABLE_NEURON "Enable ENABLE_NEURON" OFF)
option(HAVE_ROCM "Enable HAVE_ROCM" OFF)
option(HAVE_SRD "Enable HAVE_SRD" OFF)

option(HAVE_AES_XTS "Enable HAVE_AES_XTS" OFF)
option(HAVE_DCS "Enable HAVE_DCS" OFF)

# Old version of rdma-core have no ibv_query_gid_ex support
option(HAVE_GID_TYPE_DECLARED "Enable HAVE_GID_TYPE_DECLARED" OFF)

# MLNX direct verbs things, enable them if MLNX provider is installed
option(HAVE_MLX5DV "Enable HAVE_MLX5DV" OFF)
option(HAVE_MLX5DV_LIB "Enable HAVE_MLX5DV_LIB" OFF)
option(HAVE_MLX5_DEVX "Enable HAVE_MLX5_DEVX" OFF)

option(HAVE_OOO_ATR "Enable HAVE_OOO_ATR" OFF)

# There's no HAVE_SCIF in code actually
option(HAVE_SCIF "Enable HAVE_SCIF" OFF)

if("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD")
    option(IS_FREEBSD "Enable IS_FREEBSD" ON)
else()
    option(IS_FREEBSD "Enable IS_FREEBSD" OFF)
endif()

# Add config file for auto-generating config.h
configure_file(config.cmake.h.in config.h @ONLY)
add_definitions("-DHAVE_CONFIG_H")

find_package(Threads REQUIRED)


add_library(perftest STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/get_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/perftest_communication.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/perftest_parameters.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/perftest_resources.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/perftest_counters.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/host_memory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mmap_memory.c
)

if(HAVE_CUDA)
    target_sources(perftest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cuda_memory.c
    )
endif()

if(ENABLE_ROCM)
    target_sources(perftest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rocm_memory.c
    )
endif()

if(HAVE_RAW_ETH)
    target_sources(perftest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/raw_ethernet_resources.c
    )
endif()

target_link_libraries(perftest Threads::Threads ibverbs rdmacm pci rt)

if(HAVE_CUDA)
    target_link_libraries(perftest cuda)
endif()

add_executable(ib_send_bw
    ${CMAKE_CURRENT_SOURCE_DIR}/src/multicast_resources.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/send_bw.c
)
target_link_libraries(ib_send_bw perftest ibumad m)

add_executable(ib_send_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/multicast_resources.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/send_lat.c
)
target_link_libraries(ib_send_lat perftest ibumad m)

add_executable(ib_write_bw
    ${CMAKE_CURRENT_SOURCE_DIR}/src/write_bw.c
)
target_link_libraries(ib_write_bw perftest m)

add_executable(ib_write_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/write_lat.c
)
target_link_libraries(ib_write_lat perftest m)

add_executable(ib_read_bw
    ${CMAKE_CURRENT_SOURCE_DIR}/src/read_bw.c
)
target_link_libraries(ib_read_bw perftest m)

add_executable(ib_read_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/read_lat.c
)
target_link_libraries(ib_read_lat perftest m)

add_executable(ib_atomic_bw
    ${CMAKE_CURRENT_SOURCE_DIR}/src/atomic_bw.c
)
target_link_libraries(ib_atomic_bw perftest m)

add_executable(ib_atomic_lat
    ${CMAKE_CURRENT_SOURCE_DIR}/src/atomic_lat.c
)
target_link_libraries(ib_atomic_lat perftest m)

set(BINARIES
    ib_send_bw ib_send_lat
    ib_write_bw ib_write_lat
    ib_read_bw ib_read_lat
    ib_atomic_bw ib_atomic_lat
)

if(HAVE_RAW_ETH)
    add_executable(raw_ethernet_bw
        ${CMAKE_CURRENT_SOURCE_DIR}/src/raw_ethernet_send_bw.c
    )
    target_link_libraries(raw_ethernet_bw perftest m)

    add_executable(raw_ethernet_lat
        ${CMAKE_CURRENT_SOURCE_DIR}/src/raw_ethernet_send_lat.c
    )
    target_link_libraries(raw_ethernet_lat perftest m)

    add_executable(raw_ethernet_burst_lat
        ${CMAKE_CURRENT_SOURCE_DIR}/src/raw_ethernet_send_burst_lat.c
    )
    target_link_libraries(raw_ethernet_burst_lat perftest m)

    add_executable(raw_ethernet_fs_rate
        ${CMAKE_CURRENT_SOURCE_DIR}/src/raw_ethernet_fs_rate.c
    )
    target_link_libraries(raw_ethernet_fs_rate perftest m)

    set(BINARIES
        ${BINARIES} raw_ethernet_bw raw_ethernet_lat
        raw_ethernet_burst_lat raw_ethernet_fs_rate
    )
endif()

# For binary installation
foreach(binary ${BINARIES})
    install(TARGETS "${binary}" RUNTIME DESTINATION bin)
endforeach()

install(PROGRAMS "run_perftest_loopback" DESTINATION bin)
install(PROGRAMS "run_perftest_multi_devices" DESTINATION bin)

# For man pages
set(DIST_MAN "${CMAKE_CURRENT_SOURCE_DIR}/man/perftest.1")

set(MAN_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/man")

set(NON_SOURCE_MANS
    ib_send_bw.1
    ib_send_lat.1
    ib_write_bw.1
    ib_write_lat.1
    ib_read_bw.1
    ib_read_lat.1
    ib_atomic_bw.1
    ib_atomic_lat.1
    raw_ethernet_bw.1
    raw_ethernet_lat.1
    raw_ethernet_burst_lat.1
    raw_ethernet_fs_rate.1
)

foreach(man ${NON_SOURCE_MANS})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink perftest.1 ${MAN_OUTPUT_DIR}/${man}
    )
    install(FILES "${MAN_OUTPUT_DIR}/${man}" DESTINATION share/man/man1)
endforeach()

install(FILES "${DIST_MAN}" DESTINATION share/man/man1)
