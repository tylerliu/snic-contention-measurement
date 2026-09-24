# SmartNIC Contention Measurement

Measurement tools and experiment materials for **“The Hidden Cost of
Heterogeneity: Navigating Internal Contention in SoC SmartNICs.”**

Hardware throughput reproduction requires the lab topology described below.

## Environment Setup

### Testbed

The testbed uses a dual-port **NVIDIA BlueField-3 SmartNIC** and a
**NVIDIA ConnectX-7 peer NIC**, directly connected by **two 200 Gbps Ethernet
cables**. The two links provide **400 Gbps aggregate networking capacity**.
Both devices connect to the host through PCIe connections sized to sustain
their communication capacities. See the paper's testbed figure and appendix
setup table for the full hardware and software configuration.

Host and ARM workloads use NVIDIA's DOCA SDK with DPDK and accompanying
libraries to access the SmartNIC. The network tests use both ports through
a dual-port DPDK application, without link aggregation. Keep the data links
at an MTU large enough to support **RDMA MTU 4096**; set each test’s
packet size in the DPDK traffic generator.
The paper’s isolation experiments use **1500-byte MTU** / 1472-byte UDP payload. The
128-byte payload comparison is specific to message-size experiments. 

### Software and builds

The DOCA development image provides the SDK, DPDK, accompanying libraries,
and build tools used by the workloads. Build the host and ARM executables
in their respective environments. If the machine has a DOCA library version
newer than 3.1, use the DOCA 3.1 Docker containers to reproduce the experiments:

| Environment | Container image |
| --- | --- |
| Host | `nvcr.io/nvidia/doca/doca:3.1.0-devel-host` |
| BlueField ARM | `nvcr.io/nvidia/doca/doca:3.1.0-devel` |

Build and run each application in the same DOCA environment. For native
builds, install the DOCA 3.1 SDK, DPDK development libraries, and build
dependencies, and configure `PKG_CONFIG_PATH` where required.
The experiment guide for each scenario provides additional setup, build,
and launch commands.

### DOCA 3.1 Docker setup

Configure devices, scalable functions, hugepages, forwarding, and the host's
`remote` network namespace on the native systems before opening a container.
Copy the required sources to each system as described in the scenario guide.
Start from the repository root, or the copied source directory on the SoC:

```sh
case "$(uname -m)" in
  aarch64|arm64) sdk_image=nvcr.io/nvidia/doca/doca:3.1.0-devel ;;
  *) sdk_image=nvcr.io/nvidia/doca/doca:3.1.0-devel-host ;;
esac
sudo docker run --rm -it --privileged --network host \
  -v /dev:/dev -v /sys:/sys -v /dev/hugepages:/dev/hugepages \
  -v /var/run/netns:/var/run/netns \
  -v "$PWD:$PWD" -w "$PWD" "$sdk_image" bash
```

The source mount preserves the native paths. Run the scenario's build and
application commands inside this shell, omitting `sudo` because it runs as
root. Install any missing scenario dependencies inside the image. Use a fresh
build directory when switching SDK environments, and compile and execute each
binary in the same environment. Avoid mounting native SDK libraries into the
container. Record the image digest with the results.

For concurrent processes, open a container shell from the same source directory
in each terminal. Keep devices, CPU cores, and DPDK file prefixes separate.
Alternatively, add `--name NAME` to the startup command and use
`sudo docker exec -it NAME bash` from additional terminals while it is running.
Run temperature logging with `sensors` on the native SoC.

For peer traffic in the host's `remote` namespace, install `iproute2` and
`util-linux` inside the image if needed. Enter the namespace with a private
mount namespace and refresh its sysfs view:

```sh
nsenter --net=/var/run/netns/remote unshare --mount --propagation private bash
mount -t sysfs sysfs /sys
# Run the peer command here without its sudo ip netns exec remote wrapper.
```

The sysfs remount stays local to this peer shell. Run host-facing tools in
another shell in the default network namespace. Native peer tools continue
to use `sudo ip netns exec remote COMMAND`.

### Networking setup

The host sees the ConnectX-7 peer NIC and the BlueField host-facing PCIe
functions. The BlueField ARM system sees its own PFs and the representors
used to forward traffic to the host. The table lists the lab device and
interface mapping; replace these identifiers with the corresponding devices
in your testbed. PCI addresses are local to the system on which they are used.

| Device / interface | Location | Lab identifiers | IP addresses (port 0, port 1) | Role |
| --- | --- | --- | --- | --- |
| ConnectX-7 PCIe functions | Host, `remote` network namespace | `82:00.0`, `82:00.1` | `172.16.1.20`, `172.16.2.20` | Send traffic over the two physical links |
| BlueField host-facing PCIe functions | Host | `01:00.0`, `01:00.1` | `172.16.1.128`, `172.16.2.128` | Receive traffic forwarded to the host |
| BlueField PFs | ARM | `03:00.0`, `03:00.1` | `172.16.1.2`, `172.16.2.2` | Process traffic arriving on the two uplinks |
| Host representors | ARM | `pf0hpf`, `pf1hpf` | N/A | Forward traffic to the corresponding host functions |
| Scalable functions (SFs) | ARM | `en3f0pf0sf4`, `en3f1pf1sf6` | N/A | Send and receive packets from ARM cores |

Connect the two port pairs and verify that each Ethernet link is up at
200 Gbps. Verify the negotiated PCIe speed and width for both NICs on the
host. Set the data-link MTUs large enough to support RDMA MTU 4096,
including the physical ports, host
and SF representors, and the host, SoC SF, and peer data interfaces. Keep
the paths separate, without bonding or link aggregation. DPDK controls
the traffic payload size: use **`-z 1472` for the paper’s isolation
experiments**, and `-z 128` for the crypto/network concurrency size comparison. A 1472-byte UDP
payload produces a 1500-byte IPv4/UDP packet; it does not require a
1500-byte link MTU. Keep the link MTU large enough for all workloads;
select packet sizes through the traffic generator. For RDMA workloads,
verify matching selected path MTUs at every endpoint. Leave management
interfaces unchanged.

Create the host network namespace `remote` and place the ConnectX-7 peer
interfaces and their RDMA devices in it. Run peer traffic tools in this
namespace and the BlueField host-side tools in the host's default namespace.
This separates the peer NIC's network stack from the BlueField host-side
network stack while both NICs reside in the same host.

On ARM, make both PFs and their host representors available to the workloads.
Create and activate scalable functions (SFs) to provide additional interfaces
for sending and receiving packets from ARM cores. Follow NVIDIA's
[BlueField Scalable Functions setup guide (DOCA 3.1)](https://networking-docs.nvidia.com/doca/archive/3-1-0/bluefield-scalable-functions).
Use the resulting interface names in each workload's configuration and keep
the PF, host representor, and SF mapping consistent for each physical port.

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
through DOCA Comm Channel. ARM runs the transport server and the host runs
the transport client. The host selects message size, count, and traffic direction.

Requirements: a compatible host/DPU pair, Linux, a C compiler, Meson ≥ 0.61.2,
Ninja, and development packages exposing `doca-common`, `doca-argp`, and
`doca-comch` through `pkg-config` (plus libbsd where `strlcpy` is unavailable).
Use matching SDK/runtime versions on both endpoints.
Build separately on each architecture, from the repository root:

```sh
meson setup secure-channel-tester/build secure-channel-tester
meson compile -C secure-channel-tester/build
```

The source selects DPU behavior with `DOCA_ARCH_DPU`; verify that the DPU SDK
build defines it. If it does not, configure the DPU build with
`-Dc_args=-DDOCA_ARCH_DPU`. Record build type and compiler flags: this project's
Meson default is `debug`.

Start the ARM endpoint first, then the host in another terminal. Replace all angle-bracket
placeholders with the addresses from your machine; the numeric values below
are a connectivity example, not a paper measurement setting.

```sh
# ARM transport server (its own checkout and native build)
sudo ./secure-channel-tester/build/doca_secure_channel \
  -p <DPU_COMCH_PCI> -r <HOST_REPRESENTOR_PCI>

# Host transport client
sudo ./secure-channel-tester/build/doca_secure_channel \
  -p <HOST_COMCH_PCI> -n 10000 -s 65535 -d bidirectional
```

`-d send` sends from the host, `-d recv` receives at the host, and
`-d bidirectional` enables both directions. `-c` selects continuous traffic.
Retain both endpoints' logs and verify completion and message counts for a
finite run. Statistics include duration, messages/s, and bandwidth; the code
labels bandwidth `MB/s` but divides bytes by 1024² (MiB/s). The sample output
in the tool README is illustrative, not a reference result.

## Scenario 1: Lookaside Accelerator & Distributed File System

Follow the [Scenario 1 reproduction instructions](lookaside-accelerator-dfs/README.md) for isolated and concurrent AES-XTS/GCM accelerator benchmarks, network and local-load sweeps, and LiteFS throughput under host-to-network and network-to-host traffic. The included LiteFS code supports `none`, AES-XTS, and AES-GCM log processing. Report its `aes-gcm` variant as **GCM (with auth tag omitted)**.

## Scenario 2: DPA All-Reduce

Follow the [DPA all-reduce reproduction guide](dpa-all-reduce/README.md) for native SoC builds and commands, with optional DOCA 3.1 Docker shells. It specifies standalone read/write/send/receive baselines, the 44-case contention matrix, send-buffer and thread/queue sweeps, a hierarchical parameter server with 16 ARM workers, paced Host-to-network traffic, and receive capped at 50 Gib/s. The guide includes exact buffer accounting, correctness checks, and measurement limits; reproduce using the existing programs and record their source identities. Build and execute against the same FlexIO runtime.

## Scenario 3: Network Monitoring Application

The [monitoring experiments](monitoring/README.md) measure how packet cloning,
IPsec processing, and sampling compose in the BlueField NIC pipeline. With
ESP anti-replay enabled with packet cloning, a significant throughput degradation is reported in the paper.

Three separate applications reproduce the comparisons:

| Application | Comparison |
| --- | --- |
| [Cloning only](monitoring/cloning_only/) | Toggle `disable-mirroring` with fixed main and clone destinations to measure cloning overhead without IPsec. |
| [IPsec cloning](monitoring/ipsec_cloning/) | Toggle `ipsec-decap-anti-replay` while keeping the SAs, traffic, and `monitor-dest: dpdk` fixed. |
| [IPsec sampling](monitoring/ipsec_sampling/) | Sweep sampling fractions with copies sent to SFs and captured by Suricata on ARM; measure host receiving, monitor sampled, and ARM processed rates separately. |

Build and run one application at a time in the DOCA 3.1 ARM environment.
Each produces `monitoring_app` and uses its own `monitoring_app_params.yml`.
Follow the [build and launch instructions](monitoring/README.md#build-the-applications-natively)
and adapt the device mapping to your testbed. For example, from the repository
root inside the ARM build/runtime environment:

```sh
cd monitoring/ipsec_cloning
meson setup build --buildtype=release
meson compile -C build
./build/monitoring_app --config monitoring_app_params.yml
```

Start the monitoring pipeline and receiver before sending traffic. For
`cloning_only`, send UDP traffic to port 3333; its default `clone-dest: drop`
measures the mirror path without delivering copies to a monitor. For the IPsec
variants, use **`dpdk_esp_gen`** and **`dpdk_simple_recv`** from
`packet_gen_recvs/`. The supplied configurations install one inbound SA per
port, matching the shared-SA generator. Match the sender's per-port SPI, key,
salt, IV, and tunnel addressing to the selected YAML.

For the sampling application, follow the [Suricata setup and capture instructions](monitoring/README.md#build-suricata-802-on-the-soc)
to build **Suricata 8.0.2 with DPDK** and **VectorScan 5.4.12**; Suricata is
installed separately from the DOCA image. Use the supplied
[capture configuration](monitoring/suricata.yaml) and keep the anti-replay
setting fixed across each sampling sweep. Distinguish monitoring disabled
(`monitor-dest: none`) from an enabled monitoring branch with sampling fraction
zero. The SFs receive mirrored ESP before decryption, so Suricata's decoded
rate measures captured ESP traffic rather than inspection of inner payloads.

Use the [statistics procedure](monitoring/README.md#read-the-statistics) for
sample selection and conversion to Gib/s. Keep offered load, packet size,
core placement, and port mapping fixed across comparisons, and retain host RX
loss counters, Suricata drops, and timestamped NIC temperatures with the rates.

## Discussion: monitoring with ARM IPsec ESP decapsulation

**Four ARM workers achieve 183 Gib/s with cloning, IPsec ESP
decapsulation, and anti-replay enabled**, exceeding 180 Gib/s. Hardware
handles ESP authentication/decryption; ARM workers perform decapsulation and
per-SA anti-replay, bypassing the NIC pipeline feature-composition penalty
observed in Scenario 3.

This is approximately **2×** Scenario 3's **88 Gib/s**. Both
experiments use a 1500-byte link MTU, a dual-port DPDK application without
link aggregation, the same hardware setup, and the same receiver. The ARM
result uses sampling fraction 1.0 and reports 9,344 SoC RX misses over the
whole trial.

Follow the [Discussion reproduction instructions](ipsec-arm-decap/README.md)
for the ARM pipeline, exclusive-SA ESP generator, host receiver, and rate
calculation. The public artifact includes the source and configurations;
the raw logs underlying the reported reference rates are not included.

## Reproducibility record

For each reported result retain the repository revision, exact commands and
configuration, compiler/build flags, host and DPU OS/kernel, CPU and NIC/DPU
models, firmware/driver versions, DOCA/DPDK/OpenSSL versions, container digests,
port topology, MTU/link speed, hugepage allocation, core/NUMA placement, raw
logs, and the analysis output. Record all repetitions and failed runs, their
exclusion reasons, and the averaging interval. Build directories and caches
are ignored. Local run output under `reproduction-runs/` is private and
excluded from this repository.

## Record temperatures during traffic

Run `sensors` on the SoC immediately before each trial, then retain the complete sensor output throughout the traffic interval and once after shutdown. Timestamp every reading so it can be matched to the throughput snapshots:

```sh
while true; do date -u '+%Y-%m-%dT%H:%M:%SZ'; sensors; sleep 1; done | tee temperature.log
```

Start the loop on `soc` before the workload and stop it after the run. Compare each SoC NIC ASIC's measured temperature with its reported `crit` limit, and check for a sustained throughput fall as temperature rises. Temperature alone does not prove throttling; retain any device throttle counters or firmware events if available. Record the device, sensor name, time, and units with each rate.
