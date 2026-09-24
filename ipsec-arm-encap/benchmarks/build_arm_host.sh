#!/bin/bash
set -eu
export PKG_CONFIG_PATH=/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}
deps='libdpdk doca-common doca-flow doca-dpdk-bridge'
cc -O3 -include stdlib.h $(pkg-config --cflags $deps) -c dpdk_common.c utils.c
cc -O3 -DARM_HOST_DOCA $(pkg-config --cflags $deps) -c arm_simple_gen.c
c++ -O3 -DDOCA_ALLOW_EXPERIMENTAL_API $(pkg-config --cflags $deps) -c arm_host_flow.cpp device_manager.cpp
c++ -o arm_simple_gen_doca arm_simple_gen.o arm_host_flow.o device_manager.o dpdk_common.o utils.o $(pkg-config --libs $deps) -pthread
