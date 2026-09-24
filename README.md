# SmartNIC Contention Measurement

Measurement tools and experiment materials for **“The Hidden Cost of
Heterogeneity: Navigating Internal Contention in SoC SmartNICs.”**

Hardware throughput reproduction requires the lab topology described below;
the retained Discussion logs can be analyzed without a SmartNIC.

## Environment Setup

## Traffic tools

### Network traffic: `packet_gen_recvs`

[Packet generators and receivers](packet_gen_recvs/README.md) contains socket
smoke-test tools and DPDK traffic tools. The Discussion experiment uses
`dpdk_esp_exclusive_gen` and `dpdk_simple_recv`.

| Executable(s) | Purpose |
| --- | --- |
| `pkt_gen`, `pkt_recv` | UDP connectivity checks through kernel sockets |
| `aes_gcm_pkt_gen` | Socket-based AES-GCM traffic |
| `dpdk_simple_gen`, `dpdk_simple_recv` | Fixed-payload traffic generation and packet reception |
| `dpdk_aes_gcm_gen` | AES-GCM traffic generation |
| `dpdk_esp_gen` | ESP generator with shared-SA sequencing |
| `dpdk_esp_exclusive_gen` | ESP generator with one data queue and SA per TX worker |
| `dpdk_eth_pairs` | Ethernet frame pair service |
| `dpdk_kv_gen`, `dpdk_kv_latency` | Key-value traffic and latency tools |
| `dpdk_single_core_multi_queue` | Single-core, multiple-queue traffic tool |

Requirements: Linux, C/C++ compilers, Meson, Ninja, `pkg-config`, OpenSSL,
DPDK development libraries, and pthreads. The supplied Meson project resolves
DPDK even when building socket tools. Set the pkg-config path to your installed
DPDK; this example is the documented x86 host path:

```sh
meson setup packet_gen_recvs/build packet_gen_recvs \
  -Dpkg_config_path=/opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig
meson compile -C packet_gen_recvs/build
```

Build natively on each machine: the project uses `-march=native` and
`-mtune=native`. Use the DOCA 3.1 host environment for the Discussion ESP tools.
DPDK execution also requires configured hugepages, device access, available
CPU cores, and working NIC drivers/topology.

A socket smoke test requires two terminals (stop both with Ctrl-C):

```sh
# Terminal 1
./packet_gen_recvs/build/pkt_recv 3282
# Terminal 2, on the same host
./packet_gen_recvs/build/pkt_gen 127.0.0.1 3282
```

Success means the receiver reports incoming datagrams. This random-payload,
approximately 10-packet/s test establishes connectivity only.

For a DPDK link test, start RX before TX on the corresponding machines.
Replace addresses and lcores for your topology:

```sh
# Receiver: first lcore is the main core; leave a worker core available.
sudo ./packet_gen_recvs/build/dpdk_simple_recv \
  -a <RX_PCI> -l <RX_LCORES> --rx-ip <RX_IP> -p 3282 -f smoke-rx -S
# Sender
sudo ./packet_gen_recvs/build/dpdk_simple_gen \
  -a <TX_PCI> -l <TX_LCORES> -s <TX_IP> -d <RX_IP> \
  -z 512 -p 3282 -t 1000 -f smoke-tx -S
```

The simple and ESP generators require `-d` for destination IPs. Repeat per-port
options in matching order for multiple ports. Consult each executable's `--help`:
options differ between tools, and `-t` on the simple/ESP generators is a count
of `rte_pause()` calls, not a target bitrate. Use unique DPDK file prefixes
and disjoint devices/cores for independent processes. Check RX traffic and
missed/no-mbuf counters as well as TX rate.

### Host–ARM communication: `secure-channel-tester`

[DOCA Secure Channel Tester](secure-channel-tester/README.md) measures messaging
through DOCA Comm Channel. The host is the server and the ARM side of the DPU
is the client;
the host selects message size, count, and traffic direction.

Requirements: a compatible host/DPU pair, Linux, a C compiler, Meson ≥ 0.61.2,
Ninja, and development packages exposing `doca-common`, `doca-argp`, and
`doca-comch` through `pkg-config` (plus libbsd where `strlcpy` is unavailable).
The original measurement SDK version still needs author confirmation.
Build separately on each architecture, from the repository root:

```sh
meson setup secure-channel-tester/build secure-channel-tester
meson compile -C secure-channel-tester/build
```

The source selects DPU behavior with `DOCA_ARCH_DPU`; verify that the DPU SDK
build defines it. If it does not, configure the DPU build with
`-Dc_args=-DDOCA_ARCH_DPU`. Record build type and compiler flags: this project's
Meson default is `debug`.

Start the host first, then the DPU in another terminal. Replace all angle-bracket
placeholders with the addresses from your machine; the numeric values below
are a connectivity example, not a paper measurement setting.

```sh
# Host
sudo ./secure-channel-tester/build/doca_secure_channel \
  -p <HOST_COMCH_PCI> -n 10000 -s 1024 -d bidirectional

# DPU (its own checkout and native build)
sudo ./secure-channel-tester/build/doca_secure_channel \
  -p <DPU_COMCH_PCI> -r <HOST_REPRESENTOR_PCI>
```

`-d send` sends from the host, `-d recv` receives at the host, and
`-d bidirectional` enables both directions. `-c` selects continuous traffic.
Retain both endpoints' logs and verify completion and message counts for a
finite run. Statistics include duration, messages/s, and bandwidth; the code
labels bandwidth `MB/s` but divides bytes by 1024² (MiB/s). The sample output
in the tool README is illustrative, not a reference result.

## Scenario 1

## Scenario 2

## Scenario 3: Network Monitoring Application

## Discussion: monitoring with ARM IPsec ESP decapsulation

**Four ARM workers achieve 183.011821 Gib/s with cloning, IPsec ESP
decapsulation, and anti-replay enabled**, exceeding 180 Gib/s. Hardware
handles ESP authentication/decryption; ARM workers perform decapsulation and
per-SA anti-replay, bypassing the NIC pipeline feature-composition penalty
observed in Scenario 3.

This is approximately **2×** Scenario 3's **88 Gib/s**. Both
experiments use a 1500-byte link MTU, a dual-port DPDK application without
link aggregation, the same hardware setup, and the same receiver. The ARM
result uses sampling fraction 1.0 and reports 9,344 SoC RX misses over the
whole trial.

Follow the [experiment setup and build instructions](ipsec-arm-encap/README.md)
to configure the host and DPU. The runner uses DOCA 3.1 containers and requires
adapting lab paths, PCI/MAC addresses, the SSH target, and the sender network
namespace. After setup, run from the repository root on the host:

```sh
cd ipsec-arm-encap
bash benchmarks/run_decrypt_isolation.sh sweep_sf1 1550 cloning180a 16 static
bash benchmarks/summarize_decrypt_sweep.sh sweep_sf1
```

Use a fresh run label each time. Check the `cloning180a` row for clear-host RX
above 180 Gib/s and retain the TX/RX/SoC logs. Verify anti-replay configuration,
SA-to-worker placement, and monitor-output/SF counters using the experiment
guide. Each run takes roughly a minute after setup.

To analyze the retained cloning-enabled result without hardware, run only the
summary command above and inspect `sfcompare1g`. Rates average samples 8–23;
miss counters cover the whole trial. See the [result report](ipsec-arm-encap/benchmarks/ARM_ANTI_REPLAY_SF_COMPARISON.md)
for the retained logs and counter interpretation.

## Reproducibility record

For each reported result retain the repository revision, exact commands and
configuration, compiler/build flags, host and DPU OS/kernel, CPU and NIC/DPU
models, firmware/driver versions, DOCA/DPDK/OpenSSL versions, container digests,
port topology, MTU/link speed, hugepage allocation, core/NUMA placement, raw
logs, and the analysis output. Record all repetitions and failed runs, their
exclusion reasons, and the averaging interval. Build directories and caches
are ignored; benchmark logs and result data remain visible to Git.
