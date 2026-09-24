#!/usr/bin/env bash
# Run natively on the BlueField SoC after installing DOCA 3.1 and build tools.
# Sources and binaries are placed beside this script. See monitoring/README.md.
set -euo pipefail

mode="${1:-vectorscan}"
if [[ "$mode" != basic && "$mode" != vectorscan ]]; then
    echo 'Usage: setup_suricata.sh [basic|vectorscan]' >&2
    exit 2
fi
if [[ "$(id -u)" -ne 0 ]]; then
    echo 'Run with sudo on the BlueField SoC.' >&2
    exit 2
fi
build_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if ! pkg-config --exists libdpdk; then
    echo "libdpdk.pc was not found; install/configure the DOCA 3.1 DPDK SDK first." >&2
    exit 2
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential autoconf automake libtool pkg-config cmake git curl \
    rustc cargo libcap-ng-dev liblz4-dev libyaml-dev libjansson-dev \
    libpcre2-dev libmagic-dev libpcap-dev

if [[ "$mode" == vectorscan ]]; then
    apt-get install -y --no-install-recommends \
        libvectorscan-dev ragel libsqlite3-dev libboost-dev
    cargo install cbindgen --version 0.26.0 --locked
    export PATH="${CARGO_HOME:-/root/.cargo}/bin:$PATH"

    if [[ ! -d "${build_dir}/vectorscan-5.4.12/.git" ]]; then
        git clone --depth 1 --branch vectorscan/5.4.12 \
            https://github.com/VectorCamp/vectorscan.git "${build_dir}/vectorscan-5.4.12"
    fi
    cmake -S "${build_dir}/vectorscan-5.4.12" -B "${build_dir}/vectorscan-5.4.12/build" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
        -DBUILD_STATIC_LIBS=OFF -DUSE_CPU_NATIVE=ON
    cmake --build "${build_dir}/vectorscan-5.4.12/build" --target hs_shared --parallel 8
    cp -a "${build_dir}"/vectorscan-5.4.12/build/lib/libhs* /usr/local/lib/
    ldconfig
    # Debian's VectorScan 5.4.6 may still win in ldconfig's cache.
    export LD_LIBRARY_PATH="/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

if [[ ! -f "${build_dir}/suricata-8.0.2.tar.gz" ]]; then
    curl -fL -o "${build_dir}/suricata-8.0.2.tar.gz" \
        https://www.openinfosecfoundation.org/download/suricata-8.0.2.tar.gz
fi
# Use a separate clean source tree for each build; never reuse objects
# configured with a different matcher.
source_dir="${build_dir}/suricata-8.0.2-${mode}"
if [[ ! -d "$source_dir" ]]; then
    mkdir -p "$source_dir"
    tar -xzf "${build_dir}/suricata-8.0.2.tar.gz" -C "$source_dir" --strip-components=1
fi
cd "$source_dir"
if [[ -f Makefile ]]; then
    make distclean
fi

if [[ "$mode" == vectorscan ]]; then
    ./configure --enable-dpdk \
        --with-libhs-includes=/usr/include \
        --with-libhs-libraries=/usr/local/lib \
        --prefix="${build_dir}/install-hs"
else
    ./configure --enable-dpdk --prefix="${build_dir}/install"
fi
make -j8
make install

binary="${build_dir}/install/bin/suricata"
if [[ "$mode" == vectorscan ]]; then
    binary="${build_dir}/install-hs/bin/suricata"
    LD_LIBRARY_PATH="/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$binary" | grep 'libhs.so.5 => /usr/local/lib/libhs.so.5'
fi
"$binary" --build-info | grep -E 'DPDK support|Hyperscan support'
echo "Built $mode Suricata 8.0.2: $binary"
