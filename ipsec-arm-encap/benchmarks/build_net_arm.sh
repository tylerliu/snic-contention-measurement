#!/bin/bash
set -eu
export PKG_CONFIG_PATH=/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}
deps='libdpdk doca-common doca-flow doca-dpdk-bridge'
mkdir -p build-net-arm
cc -O3 -DDOCA_ALLOW_EXPERIMENTAL_API -include stdlib.h $(pkg-config --cflags $deps) -c dpdk_common.c -o build-net-arm/common.o
cc -O3 -DDOCA_ALLOW_EXPERIMENTAL_API $(pkg-config --cflags $deps) -c utils.c -o build-net-arm/utils.o
c++ -std=c++17 -O3 -DDOCA_ALLOW_EXPERIMENTAL_API $(pkg-config --cflags $deps) -c device_manager.cpp -o build-net-arm/devices.o
c++ -std=c++17 -O3 -DDOCA_ALLOW_EXPERIMENTAL_API $(pkg-config --cflags $deps) net_arm_forward.cpp \
 build-net-arm/common.o build-net-arm/utils.o build-net-arm/devices.o $(pkg-config --libs $deps) -pthread -o build-net-arm/net_arm_forward
