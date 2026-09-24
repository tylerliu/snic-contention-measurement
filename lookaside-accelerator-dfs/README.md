# Reproduce the lookaside accelerator and LiteFS experiments

Use the dual-port BlueField-3/ConnectX-7 topology and device mapping in the [repository setup](../README.md#environment-setup). Run the commands below in a shell with DOCA 3.1 and DPDK installed on the host or BlueField ARM system. You can use a native installation or the optional development shell below. Replace the lab addresses, device names, cores, and SSH alias `soc` with your own.

Set the data-link MTUs large enough to support **RDMA MTU 4096** throughout the physical ports, representors, and endpoints. Keep those MTUs across the Scenario 1 experiments. Use **1472-byte UDP payloads for the paper's isolation experiments**; 128-byte payloads are used only in the crypto/network concurrency size comparison. LiteFS concurrent traffic uses 1472-byte payloads in both directions. DPDK selects packet size with `-z`, independently of the link MTU.

## Optional DOCA development shell

Use the shared [DOCA 3.1 Docker setup](../README.md#doca-31-docker-setup). On the host, start from the repository root. On the SoC, copy the accelerator and LiteFS sources as described below, and copy the traffic and secure-channel sources into that same source directory before opening the shell. Start the SoC shell from the copied `lookaside-accelerator-dfs` directory. The shared mount preserves its absolute path; use that path for `/path/to/lookaside-accelerator-dfs` below. Install any missing dependencies listed in the next section inside the image.

## Native setup and builds

Install Meson, Ninja, a C/C++ compiler, Python 3, pkg-config, the DOCA 3.1 SDK and DPDK development libraries, libibverbs, libyaml development headers, and the traffic tools' OpenSSL dependencies. LiteFS requires DPDK >= 22.11, `doca-common`, `doca-dma`, and `doca-dpdk-bridge`. Configure hugepages and access to the NIC/RDMA devices on both machines; verify that the hugepage pools are allocated before running EAL.

Set the native SDK pkg-config path on each machine if required:

```sh
sdk_arch="$(uname -m)-linux-gnu"
export PKG_CONFIG_PATH="/opt/mellanox/dpdk/lib/$sdk_arch/pkgconfig:/opt/mellanox/doca/lib/$sdk_arch/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --modversion libdpdk
```

From the repository root on the host, copy the accelerator and LiteFS sources to the SoC:

```sh
rsync -a --exclude build --exclude 'build-*' lookaside-accelerator-dfs/ \
  soc:~/lookaside-accelerator-dfs/
```

In a SoC shell, set the source path to the copied directory (`~/lookaside-accelerator-dfs` for the documented copy), then build the ARM binaries:

```sh
cd /path/to/lookaside-accelerator-dfs
meson setup accelerator-bench/build accelerator-bench --buildtype=release
meson compile -C accelerator-bench/build
meson setup LiteFS/build LiteFS --buildtype=release -Denable_crypto=true
meson compile -C LiteFS/build
```

From the repository root on the host, build the native traffic and LiteFS binaries:

```sh
meson setup packet_gen_recvs/build packet_gen_recvs --buildtype=release
meson compile -C packet_gen_recvs/build \
  dpdk_simple_gen dpdk_simple_recv dpdk_single_core_multi_queue
meson setup lookaside-accelerator-dfs/LiteFS/build lookaside-accelerator-dfs/LiteFS \
  --buildtype=release -Denable_crypto=true
meson compile -C lookaside-accelerator-dfs/LiteFS/build
```

Use `meson compile` alone for subsequent builds. Keep the native SDK, build type, and binaries fixed within a comparison. The ConnectX-7 peer interfaces and RDMA devices must be in the host's `remote` network namespace, as described in the repository setup. Run peer tools with `sudo ip netns exec remote`.

## Experiment matrix

Report AES-XTS and GCM **decryption**. Encryption benchmarks are available in `accelerator-bench/benchmark_cases.json`.

| Experiment | Settings | Cases |
| --- | --- | ---: |
| Network only | Six directed paths, without crypto | 6 |
| Crypto only | Two algorithms × four buffer sizes | 8 |
| Concurrent directions | Six paths × two algorithms; 32000-byte crypto buffers | 12 |
| Concurrent size comparison | Host-to-network; two algorithms × four crypto sizes × two UDP payload sizes | 16 |
| Single-core multi-queue | Host-to-network; two algorithms × 1/2/4/8 TX queues; 32000-byte crypto buffers | 8 |
| LiteFS | Three variants × three traffic conditions | 9 |

The six paths are **Host→Network, Network→Host, ARM→Network, Network→ARM, Host→ARM, and ARM→Host**. Use `-z 1472` for DPDK direction baselines and concurrent direction tests. The size comparison crosses crypto sizes **512, 2048, 8192, and 32000 bytes** with **both** UDP payload sizes **128 and 1472 bytes**, using host-to-network traffic only. The multi-queue comparison also uses host-to-network traffic only, with 1472-byte UDP payloads. Record matching network-only controls for both size-comparison payloads and each TX queue count. The matrix contains 59 cases; additional payload and queue controls support the comparisons. For a complete reproduction, run every listed case and its matching controls in a fresh result directory, after verifying the six direction baselines.

## Isolated decryption baselines

Run each algorithm at all four buffer sizes, with no traffic workload:

| Algorithm | Executable in `accelerator-bench/build/` | Crypto allowlist | Crypto queues |
| --- | --- | --- | --- |
| AES-XTS | `rte_cryptodev_enqueue_dequeue_burst_xts_decrypt` | `03:00.0,class=crypto,algo=0` | 3 |
| GCM | `rte_cryptodev_enqueue_dequeue_burst_decrypt` | `03:00.0,class=crypto,algo=1` | 2 at 512 bytes; otherwise 1 |

In a SoC shell:

```sh
cd /path/to/lookaside-accelerator-dfs/accelerator-bench
sudo stdbuf -oL ./build/rte_cryptodev_enqueue_dequeue_burst_xts_decrypt \
  -l 3 -a 03:00.0,class=crypto,algo=0 --file-prefix crypto-xts \
  -- --burst_size 64 --data_size 32000 --num_queues 3 --duration 20
```

For GCM, substitute the GCM executable, `algo=1`, and its queue count. Repeat both commands with each crypto buffer size. These binaries use `--duration`. Calculate Gib/s as `Total operations * data_size * 8 / (duration * 2^30)` and retain raw operation totals, cycles, and wait cycles. GCM decryption's configured buffer includes the 16-byte authentication tag. XTS counts enqueues during the interval and drains pending completions afterward. The benchmarks do not check every completion status.

## Verify six traffic baselines

Run each path without crypto first. Start the receiver before its sender, verify sustained positive received throughput at the intended endpoint, and inspect per-port RX and missed/no-mbuf counters. Discard startup and retain at least five stable received-rate samples. Use separate logs and unique DPDK file prefixes for each path. Resolve forwarding or device-selection errors before measuring concurrent crypto.

### Host-to-network

From the repository root in two host terminals:

```sh
sudo ip netns exec remote ./packet_gen_recvs/build/dpdk_simple_recv \
  -a 82:00.0 -a 82:00.1 -l 16-20 \
  --rx-ip 172.16.1.20 --rx-ip 172.16.2.20 \
  -p 3333 -S -f lookaside-rx
```

```sh
sudo ./packet_gen_recvs/build/dpdk_simple_gen \
  -a 01:00.0 -s 172.16.1.128 -d 172.16.1.20 \
  -a 01:00.1 -s 172.16.2.128 -d 172.16.2.20 \
  -l 2-6 -p 3333 -z 1472 -t 0 -S -f lookaside-tx
```

### Network-to-host

Start the host receiver, then the peer sender:

```sh
sudo ./packet_gen_recvs/build/dpdk_simple_recv \
  -a 01:00.0 -a 01:00.1 -l 16-20 \
  --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 \
  -p 3333 -S -f lookaside-host-rx
```

```sh
sudo ip netns exec remote ./packet_gen_recvs/build/dpdk_simple_gen \
  -a 82:00.0 -s 172.16.1.20 -d 172.16.1.128 \
  -a 82:00.1 -s 172.16.2.20 -d 172.16.2.128 \
  -l 2-6 -p 3333 -z 1472 -t 0 -S -f lookaside-peer-tx
```

### ARM-to-network and network-to-ARM

Build `packet_gen_recvs` on ARM as well. Copy it from the host repository root:

```sh
rsync -a --exclude build --exclude 'build-*' packet_gen_recvs/ \
  soc:~/lookaside-accelerator-dfs/packet_gen_recvs/
```

In the copied ARM source directory:

```sh
cd /path/to/lookaside-accelerator-dfs
meson setup packet_gen_recvs/build packet_gen_recvs --buildtype=release
meson compile -C packet_gen_recvs/build dpdk_simple_gen dpdk_simple_recv
```

Select the ARM DPDK endpoints connected to the two uplinks. Use the SF device allowlists corresponding to your configured SFs, and verify the forwarding path to those SFs. Set `ARM_PORT0` and `ARM_PORT1` to the verified DPDK allowlist strings in the ARM terminal. The lab mappings are:

```sh
ARM_PORT0=auxiliary:mlx5_core.sf.2
ARM_PORT1=auxiliary:mlx5_core.sf.3
```
 These endpoints use `172.16.1.2` and `172.16.2.2` in this lab; replace addresses consistently for your setup. Use four ARM traffic workers across the two ports. In the commands below, core 4 is the main core and cores 5–8 are traffic workers; keep crypto on core 3. Verify the startup worker counts.

For ARM-to-network, start the peer receiver from the host-to-network example, then run on ARM from its source root:

```sh
sudo ./packet_gen_recvs/build/dpdk_simple_gen \
  -a "$ARM_PORT0" -s 172.16.1.2 -d 172.16.1.20 \
  -a "$ARM_PORT1" -s 172.16.2.2 -d 172.16.2.20 \
  -l 4-8 -p 3333 -z 1472 -t 0 -S -f lookaside-arm-tx
```

For network-to-ARM, start the ARM receiver:

```sh
sudo ./packet_gen_recvs/build/dpdk_simple_recv \
  -a "$ARM_PORT0" -a "$ARM_PORT1" -l 4-8 -W 4 \
  --rx-ip 172.16.1.2 --rx-ip 172.16.2.2 \
  -p 3333 -S -f lookaside-arm-rx
```

Then run the peer sender on the host:

```sh
sudo ip netns exec remote ./packet_gen_recvs/build/dpdk_simple_gen \
  -a 82:00.0 -s 172.16.1.20 -d 172.16.1.2 \
  -a 82:00.1 -s 172.16.2.20 -d 172.16.2.2 \
  -l 2-6 -p 3333 -z 1472 -t 0 -S -f lookaside-peer-arm-tx
```

### Host-to-ARM and ARM-to-host

Build `secure-channel-tester` on both machines using the [host–ARM setup instructions](../README.md#hostarm-communication-secure-channel-tester). Copy its sources to ARM alongside the traffic sources:

```sh
rsync -a --exclude build --exclude 'build-*' secure-channel-tester/ \
  soc:~/lookaside-accelerator-dfs/secure-channel-tester/
```

Use its native DOCA Comm Channel device and host representor mapping. Replace the PCI placeholders below with verified devices. Keep its ARM threads on cores other than crypto core 3.

For host-to-ARM, start the ARM transport server first in its checkout:

```sh
sudo taskset -c 4-8 ./secure-channel-tester/build/doca_secure_channel \
  -p <DPU_COMCH_PCI> -r <HOST_REPRESENTOR_PCI>
```

Then start the host client:

```sh
sudo ./secure-channel-tester/build/doca_secure_channel \
  -p <HOST_COMCH_PCI> -s 65535 -d send -c
```

For ARM-to-host, restart both endpoints, using this host client command and the same ARM server command:

```sh
sudo ./secure-channel-tester/build/doca_secure_channel \
  -p <HOST_COMCH_PCI> -s 65535 -d recv -c
```

Use the **consumer** interval bandwidth from the receiving endpoint: ARM for host-to-ARM and host for ARM-to-host. Confirm the direction and receive message counts in both logs.

## Concurrent directions and size comparison

After validating each direction's network-only baseline, start the same traffic configuration and let it stabilize. Run AES-XTS decryption and GCM decryption separately on ARM with `--data_size 32000 --duration 20`, using the isolated commands above. Keep traffic active until crypto completes naturally and prints final operation totals. Repeat for all six directions: **12 cases**.

For the host-to-network size comparison, repeat both algorithms at **every combination** of the four crypto sizes and two UDP payload sizes: **16 cases**. Change the host sender's `-z` to 128 or 1472; select the crypto queues from the isolated table. Keep ports, worker cores, pacing, and forwarding identical to the matching payload's network-only baseline.

Capture timestamped receiver output and crypto output on a common clock. Select complete received-rate intervals contained within the crypto active interval, after traffic startup, and retain the interval boundaries and sample indices. For secure channel, use the consumer's actual reported interval duration. Report **crypto Gib/s and its reduction beside traffic RX Gib/s and its reduction**:

- Crypto reduction: `(isolated_crypto - concurrent_crypto) / isolated_crypto * 100`.
- Traffic RX reduction: `(baseline_RX - concurrent_RX) / baseline_RX * 100`.

DPDK and secure-channel tools label MiB/s as `MB/s`; divide by 128 for Gib/s. Retain both endpoint logs and RX counters, while reporting network throughput from the receiver. If a separate network measurement needs a long crypto job, use `--duration 3600` and interrupt it after five stable RX samples; such an interrupted run has no crypto total and cannot supply concurrent crypto Gib/s.

The supplied `network_throughput_cases.json` and `concurrent_sensitivity_cases.json` cover older subsets and executable paths. Update their resolved commands and cases before using the runners for this full matrix, and inspect `--dry-run` output.

## Single-core multi-queue experiment

Use both host TX ports, with one TX worker per port and **1, 2, 4, and 8 TX queues per port**. Start the dual-port peer receiver from the host-to-network example. Verify positive RX on both ports and an aggregate network-only rate near 366 Gib/s before running crypto. On the host:

```sh
sudo ./packet_gen_recvs/build/dpdk_single_core_multi_queue \
  -a 01:00.0 -s 172.16.1.128 -d 172.16.1.20 \
  -a 01:00.1 -s 172.16.2.128 -d 172.16.2.20 \
  -l 2-4 --tx-queues 1 -p 3333 -z 1472 -t 0 -S -f lookaside-mq
```

Core 2 is the main core; host cores 3 and 4 each serve one TX port and cycle through that port’s queues. For each queue count, measure a matching network-only baseline, then finite AES-XTS and GCM decryption runs with 32000-byte buffers while traffic stays active: **eight loaded cases**. Crypto queue settings remain fixed at three for XTS and one for GCM. Report paired crypto and traffic RX results using the procedure above. Keep both ports and both workers active for every queue control and loaded case.


## Optional local accelerator contention

On the SoC, the supplied concurrent groups pair XTS with another crypto device and XTS/GCM with Deflate or LZ4 decompression:

```sh
cd /path/to/lookaside-accelerator-dfs/accelerator-bench
python3 generate_compressed_data.py --output-dir compressed_data \
  --data-sizes 512 2048 8192 32000 --data-types random text \
  --algorithms deflate lz4 null
sudo python3 run_concurrent_throughput.py --scenario concurrent_scenarios.json
```

Compare each job with its isolated run at the same data size and queue count. Keep jobs on separate ARM cores with unique DPDK file prefixes, as in the supplied groups. Retain operation totals from raw output as well as the runner's CSV.

## LiteFS with concurrent traffic

LiteFS is a stripped-down version of [LineFS](https://github.com/casys-kaist/LineFS) for measuring host-to-SmartNIC log processing and replication. Use `none`, AES-XTS, and **GCM (with auth tag omitted)**. Its GCM processor retains ciphertext and omits the authentication tag.

Copy `nicfs_params.yml` into one run-specific YAML per mode on the SoC. Set `program_flags.log-encryption` to `none`, `aes-xts`, or `aes-gcm`; XTS uses the crypto allowlist `algo=0`, while GCM requires `algo=1`. Update PCI/IP addresses, verbs device names, replicas, and cores. Update `hostfs_params.yml` and `nic_tester_params.yml` on the host. Verbs names are local to each machine and namespace; verify them with `ibv_devinfo`, including `sudo ip netns exec remote ibv_devinfo` for the peer.

Start the server first, in a SoC shell:

```sh
cd /path/to/lookaside-accelerator-dfs/LiteFS
sudo stdbuf -oL ./build/nicfs/nicfs --config nicfs_params.yml
```

Then connect the host and replica in separate native host terminals, from the LiteFS directory:

```sh
cd lookaside-accelerator-dfs/LiteFS
sudo stdbuf -oL ./build/hostfs/hostfs --config hostfs_params.yml
```

```sh
cd lookaside-accelerator-dfs/LiteFS
sudo ip netns exec remote stdbuf -oL ./build/nic_tester/nic_tester \
  --config nic_tester_params.yml
```

Use the selected mode's YAML in the server command. Wait for all TCP/RDMA handshakes and sustained nonzero tester `STATS` before starting traffic. All three component logs must select RDMA MTU 4096. The code uses each local port's active MTU without negotiating the peer minimum; mixed path MTUs caused earlier failures. The host workload continuously generates synthetic log batches from 256 KiB to just under 8 MiB and does not read `workload.conf`.

### LiteFS traffic experiment grid

The LiteFS experiment consists of exactly **nine cases**: three variants × three cross-traffic conditions. All cross-traffic uses 1472-byte UDP payloads.

| Variant | No cross-traffic | Host-to-network | Network-to-host |
| --- | --- | --- | --- |
| No encryption (`none`) | LiteFS alone | Host TX, peer RX | Peer TX, host RX |
| AES-XTS (`aes-xts`) | LiteFS alone | Host TX, peer RX | Peer TX, host RX |
| GCM (with auth tag omitted) (`aes-gcm`) | LiteFS alone | Host TX, peer RX | Peer TX, host RX |

For host-to-network, use the native receiver/sender commands above. For network-to-host, start the following receiver on the host, from the repository root, after LiteFS bootstrap:

```sh
sudo ./packet_gen_recvs/build/dpdk_simple_recv \
  -a 01:00.0 -a 01:00.1 -l 16-20 \
  --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 \
  -p 3333 -S -f litefs-host-rx
```

Then start the peer sender:

```sh
sudo ip netns exec remote ./packet_gen_recvs/build/dpdk_simple_gen \
  -a 82:00.0 -s 172.16.1.20 -d 172.16.1.128 \
  -a 82:00.1 -s 172.16.2.20 -d 172.16.2.128 \
  -l 2-6 -p 3333 -z 1472 -t 100 -S -f litefs-peer-tx
```

Keep the same selected pacing across variants within each traffic condition and record it. The host-to-network example uses `-t 0`, while the reverse example shows `-t 100`; choose and retain a fixed setting for each direction. The sender's `-t` counts pause calls and is not a bitrate. If using explicit destination MACs to bypass ARP, verify and record them for your devices.

Discard startup and measure tester, TX, and RX rates over a common stable interval. Record actual network load, missed/no-mbuf counters, and current ASIC temperatures. Convert tester B/s to Gib/s with `B/s * 8 / 2^30`. Tester `STATS` measures replicated log throughput, rather than a finite completed-write count. Compare each loaded point with the same mode's no-traffic baseline, and compare modes at matched actual received load. Identical pacing can produce different actual traffic rates.

Stop traffic before stopping the LiteFS processes; retain all logs. `timeout -s INT -k 5 <seconds>` can bound a run. Use fresh configs/logs per point and the cooldown procedure below.

## Temperature logging and cooldown

Follow the [monitoring temperature procedure](../monitoring/README.md#record-temperatures-during-traffic). On the SoC, save a timestamped reading before each trial:

```sh
date -u +%FT%TZ
sensors | sed -n '/mlx5-pci-0300/,+2p;/mlx5-pci-0301/,+2p'
```

In a second SoC terminal, start the one-second log before the workload:

```sh
while true; do
  date -u +%FT%TZ
  sensors | sed -n '/mlx5-pci-0300/,+2p;/mlx5-pci-0301/,+2p'
  sleep 1
done | tee soc-nic-temperature.log
```

Stop it after traffic and applications shut down, and save a fresh post-run reading. Wait about 60 seconds with workloads stopped before the next large run, check temperatures, and wait longer if needed. Retain current readings separately from the sensor's historical `highest` field. Compare against the critical limit reported by your device; temperature alone does not establish throttling.
