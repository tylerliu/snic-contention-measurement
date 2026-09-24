# Reproduce the DPA and hierarchical parameter-server experiments

Use the BlueField-3/ConnectX-7 topology in the [repository setup](../README.md#environment-setup). This guide is written for **native execution on the SoC** with DOCA 3.1/FlexIO installed. Host traffic tools run on the host, and network-peer tools run in its `remote` namespace. Replace device identifiers, IP/MAC addresses, source paths, and CPU assignments with your testbed's values. Keep both 200-Gbps links available and data-link MTUs large enough for RDMA MTU 4096.

The same commands can run in the [optional Docker shells](#optional-doca-31-docker-shells). Compile and execute against matching DOCA 3.1/FlexIO installations. A binary built with DOCA 3.1 may fail against a newer native runtime; use the matching container environment before considering firmware changes.

The seven experiments below define the reproduction matrix. The
[implementation checks](#implementation-checks-before-measurement) describe
startup, counter interpretation, and measurement limits. Record the revision
and source/binary hashes with each result. Document any compatibility patches
and report results from modified workloads separately.

Use `packet_gen_recvs/dpdk_kv_gen.c` and its `dpdk_common.c`/header dependencies
for generation, combined TX/RX, and RX-only sinks. Its source-port controls,
destination RX reports, fixed `rte_pause()` pacing, and worker assignments
define the traffic workload.

For send/receive, use the selected two-port topology: **256 threads total, 128 per PF** at the main point, with send-buffer sizes counted across both instances. Read/write remain a single 256-thread workload with stride 1. Use matching controls whenever topology changes. The hierarchical server keeps its own verified single-PF branch layout.

Parameters in the experiment matrix define the workload. Device names, addresses, paths and CPU lists below are examples to replace with your environment. Record machine-specific calibration, search observations, failures and selected pacing in the result directory; those records are not additional requirements of this guide.

## Required experiment matrix

| Experiment | Required runs |
| --- | --- |
| 1. Standalone | Read, write, send, receive × DPA/ARM memory, at 256 threads: eight baselines. Additionally sweep send/receive thread counts for both memory locations. Send storage: 1.5 MiB total; receive storage: 256 KiB per thread. |
| 2. Contention | Each of the eight combinations × six traffic directions, excluding both receive-memory variants with Net→Host and Net→ARM: **44 configurations**. Also collect all six standalone flow baselines. |
| 3. Send storage | 16 total DPA threads × both memory locations × seven total buffer sizes (1.5, 2, 3, 4, 8, 12, 16 MiB) × standalone plus all six competing flows: **98 configurations**. |
| 4. Threads/worker queues | Host→Net competitor from multithreaded original KV; DPA-memory send buffers; both 1.5 and 8 MiB. Sweep DPA threads with fixed host workers/queues, then host workers and their queues with fixed DPA threads. Include matching standalone controls. |
| 5. Hierarchical server | DPA workload fractions **0, 10, …, 100%**, both DPA and ARM packet-buffer locations. Start with **16 ARM service workers**. Measure ARM, DPA, and aggregate **service RX throughput** at saturation. |
| 6. Paced traffic | Sweep competing rate while measuring DPA reads from each memory region; include no-traffic baselines. |
| 7. Capped receive | Target DPA receive rate **50 Gib/s**, each memory location × Host→Net, ARM→Net, Host→ARM, ARM→Host: **eight concurrent configurations**, plus capped standalone controls. |

The six directions are **Host→Net, Net→Host, ARM→Net, Net→ARM, Host→ARM, ARM→Host**. Read/write/send each contribute 12 configurations to Experiment 2; receive contributes eight: `3 × 12 + 8 = 44`. An exclusion is not a zero-throughput result.

## Programs and native builds

| Project | Executable under its build directory | Controls |
| --- | --- | --- |
| `membench` | `host/flexio_packet_processor` | `--op read|write`, `--threads`, `--mem_size`, `--loop`, `--stride`, `--use_private` |
| `sender` | `host/flexio_packet_processor` | `-t` threads, `-a` ARM send storage, `-k` shared packet slots, `-z` UDP payload bytes, `-p` destination port, `-i` destination IP |
| `receiver` | `host/flexio_packet_processor` | `-t` threads, `-a` ARM receive storage, `-p` destination port |
| `all-reduce-app` | `host/flexio_packet_processor` | `-t` threads, `-a` ARM packet storage, `-k` aggregation threshold, `-p` destination port |
| `all-reduce-app/dpdk_mapreduce` | `dpdk_mapreduce` | ARM service workers via `-l`, `--limit` aggregation threshold, `-p` destination port |

`sender -k` selects buffer slots; `all-reduce-app -k` selects aggregation count. These similarly named executables and options are not interchangeable. Memory location refers to packet/data storage, not necessarily queue descriptors or aggregation state. `-s` on the DPA sender/aggregator is parsed but ignored. Preserve the original sender source ports: each thread initializes its shared-pool slice using its host-assigned source port. All sender threads then cycle through the shared pool. Do not rewrite this distribution to keep it constant across thread or buffer sweeps; record the distribution produced by the original code.

Install native DOCA 3.1 including FlexIO and DPACC, Meson/Ninja, C/C++ compilers, pkg-config, RDMA development libraries, `libgflags-dev`, and the packet tools' OpenSSL dependencies. DPDK tools need hugepages. Build independently on each architecture; do not copy x86 executables to ARM.

From the repository root on the host, synchronize source to a separate SoC directory:

```sh
DPU_SSH=user@dpu-management-address
DPU_SOURCE=/path/to/snic-contention-measurement
ssh "$DPU_SSH" "mkdir -p '$DPU_SOURCE'"
rsync -a --exclude 'build*' dpa-all-reduce packet_gen_recvs secure-channel-tester \
  "$DPU_SSH:$DPU_SOURCE/"
```

On the SoC, from that source root:

```sh
REPO=/path/to/snic-contention-measurement
cd "$REPO"
export PKG_CONFIG_PATH="/opt/mellanox/flexio/lib/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --modversion libflexio libdpdk gflags
/opt/mellanox/doca/tools/dpacc --version

for app in membench sender receiver all-reduce-app; do
  meson setup "dpa-all-reduce/$app/build-doca31" "dpa-all-reduce/$app" --buildtype=release
  meson compile -C "dpa-all-reduce/$app/build-doca31"
done
meson setup dpa-all-reduce/all-reduce-app/dpdk_mapreduce/build-doca31 \
  dpa-all-reduce/all-reduce-app/dpdk_mapreduce --buildtype=release
meson compile -C dpa-all-reduce/all-reduce-app/dpdk_mapreduce/build-doca31
meson setup packet_gen_recvs/build-dpa packet_gen_recvs --buildtype=release
meson compile -C packet_gen_recvs/build-dpa dpdk_kv_gen
meson setup secure-channel-tester/build-dpa secure-channel-tester \
  --buildtype=release -Dc_args=-DDOCA_ARCH_DPU
meson compile -C secure-channel-tester/build-dpa
```

The default DPA CPU target is `bf3`. On the host, build the packet toolkit and secure-channel tester in separate native build directories, omitting `-DDOCA_ARCH_DPU`. Use `meson compile` for subsequent builds. Check `file` and `ldd` for the architecture and SDK selected by every binary.

For memory performance, verify the actual device command in `build-doca31/build.ninja`: verify release compilation uses **`-O3`, `-mcpu=nv-dpa-bf3` and device `-flto`**. Host compilation also uses `-O3`. Meson’s host `b_lto=false` does not disable the custom DPACC device LTO command. Record compiler commands, not only the requested build type.

Choose memory iterations so completed batches report approximately once per second. Keep iterations fixed between a case and its matched standalone control. Retain both the summed per-thread device-cycle bandwidth (assuming 1.8 GHz) and completed accessed bytes divided by batch wall time; these are different metrics. Use stride 1 for all required memory measurements.

## Optional DOCA 3.1 Docker shells

Use the shared [DOCA 3.1 Docker setup](../README.md#doca-31-docker-setup) from the source root on each system. The source mount preserves the paths used below. Compile and execute the DPA applications against the same FlexIO runtime.

Inside the ARM image, install `libgflags-dev` for **Ubuntu 22.04**, its base distribution. If ARM package downloads fail, obtain Ubuntu Jammy ARM64 `libgflags2.2` and `libgflags-dev` version `2.2.2-2` on a connected system, copy both into the container, and use `dpkg -i`. Ubuntu 24.04 packages `2.2.2-2build1` require newer libc/libstdc++ and are incompatible.

Record firmware, DOCA, FlexIO and DPACC versions. Errors such as `Allocate a buffer for DPA security attributes failed`, registration failures or `Failed to get Flex IO app` can indicate a mismatched runtime. Verify compilation and execution use the same SDK/runtime before changing firmware or authentication settings.

## Parameters and buffer accounting

On the SoC, define these shell variables after verifying device mappings:

```sh
cd "$REPO"
DPA_DEVICE=mlx5_0                 # Verify the associated physical PF.
DPA_PORT=3333
CROSS_PORT=3334
DPA_DEST_IP=192.0.2.20           # Documentation example; replace with peer address.
DPA_MAC=02:42:7e:7f:eb:02         # Outgoing DPA source MAC; match peer filtering.
DPA_PF_MAC=REPLACE_WITH_PF_MAC     # For traffic sent directly to the DPA PF.
MEM=dpa                          # dpa or arm
THREADS=256
SEND_SLOTS=768                    # 768 * 2048 = 1.5 MiB TOTAL.
PAYLOAD=1472                     # Fixed UDP payload for send/receive.
MEM_BYTES=1048576                # Read/write bytes per thread.
MEM_LOOPS=64                     # Read/write loop count.
MEM_STRIDE=1                     # Required main-run setting: sequential 64-bit accesses.
```

For read/write, use 1 MiB per thread, 64 loops, stride 1 and continuous mode. Keep geometry and loop count fixed between standalone and competing-traffic cases. At 256 threads this requests 256 MiB; the program default of 16 MiB per thread instead requests 4 GiB.

| Setting | Formula | Required value |
| --- | --- | --- |
| Send storage | Sum `sender -k` slots across instances × 2048 bytes/slot | 768 total slots = 1.5 MiB; two ports use 384 slots each |
| Receive storage | `2^LOG_RQ_DEPTH × 2^LOG_DATA_ENTRY_BSIZE`, currently `128 × 2048` | 262,144 bytes = 256 KiB **per thread**; 64 MiB at 256 threads |
| Read/write storage | `--mem_size × --threads` | 1 MiB/thread, set explicitly |

Receive's entry-size constant lives in `receiver/common/host/flexio_queue_wrappers.cpp`; its depth is in `receiver/flexio_packet_processor_com.h`. No buffer-size CLI change is needed for the required receive allocation. Queue metadata, SQ/RQ descriptors, and other allocations are additional to the data-storage numbers. Changing `-z` changes wire payload, not the size of each 2048-byte send slot. Omitting sender `-k` allocates `128 × threads` slots, **64 MiB at 256 threads**, rather than the required 1.5 MiB.

Define a native SoC launch helper:

```sh
run_dpa() {
  local operation=$1 memory=$2 threads=$3 slots=${4:-768}
  local memory_args=()
  case "$memory" in
    dpa) ;;
    arm) ;;
    *) echo 'memory must be dpa or arm' >&2; return 2 ;;
  esac
  case "$operation" in
    read|write)
      [ "$memory" = dpa ] && memory_args=(--use_private)
      sudo stdbuf -oL -eL dpa-all-reduce/membench/build-doca31/host/flexio_packet_processor \
        --device "$DPA_DEVICE" --threads "$threads" --op "$operation" \
        --mem_size "$MEM_BYTES" --loop "$MEM_LOOPS" --stride "$MEM_STRIDE" --continuous \
        "${memory_args[@]}"
      ;;
    send)
      [ "$memory" = arm ] && memory_args=(-a)
      sudo stdbuf -oL -eL dpa-all-reduce/sender/build-doca31/host/flexio_packet_processor \
        "$DPA_DEVICE" -t "$threads" -k "$slots" -z "$PAYLOAD" \
        -p "$DPA_PORT" -i "$DPA_DEST_IP" -m "$DPA_MAC" "${memory_args[@]}"
      ;;
    receive)
      [ "$memory" = arm ] && memory_args=(-a)
      sudo stdbuf -oL -eL dpa-all-reduce/receiver/build-doca31/host/flexio_packet_processor \
        "$DPA_DEVICE" -t "$threads" -p "$DPA_PORT" "${memory_args[@]}"
      ;;
    *) echo 'operation must be read, write, send, or receive' >&2; return 2 ;;
  esac
}
```

Use Bash for this single-instance helper; use the two-port launch templates for the selected network topology. Examples: `run_dpa send dpa 256 768`, `run_dpa receive arm 256`, `run_dpa read dpa 256`, `run_dpa write arm 256`. The helper launches every workload continuously. Capture output through `tee` or redirected files. Bound the continuous measurement window; wrap the actual application command in a bounded `timeout --signal=INT --kill-after=3s DURATION` or record and terminate its owned PID. Signal handling differs between programs; verify each process has exited afterward.

## Traffic endpoints and six standalone flows

Use port **3334** for cross traffic and **3333** for DPA traffic. This separation prevents the DPA UDP rules from intercepting the cross-flow workload. Keep source-port distributions and worker/queue counts fixed except when an experiment explicitly sweeps them.

The following addresses are documentation examples, not a live testbed mapping. In each relevant host/SoC terminal, set `REPO`, then define arrays for the endpoints available there:

```sh
# Host terminal, repository root.
HOST_DEV=(01:00.0 01:00.1)
NET_DEV=(82:00.0 82:00.1)
HOST_IP=(192.0.2.128 198.51.100.128)
NET_IP=(192.0.2.20 198.51.100.20)
ARM_IP=(192.0.2.2 198.51.100.2)
# SoC terminal, source root.
ARM_DEV=(auxiliary:mlx5_core.sf.2 auxiliary:mlx5_core.sf.3)
ARM_IP=(192.0.2.2 198.51.100.2)
NET_IP=(192.0.2.20 198.51.100.20)
```

Physical PFs serve DPA workloads; use SFs for ARM network cross traffic. Verify the PF/SF forwarding relationship. Reserve ARM core 3 for the DPA control process where practical, cores 4–12 for network workers, and separate cores for service workers. CPU affinity is for ARM software, not a DPA-thread count. The supplied core ranges are examples; document NUMA placement and overlap explicitly.

Check host topology with `lscpu -e=CPU,CORE,SOCKET,NODE`; different logical CPU numbers can be SMT siblings sharing one physical core. Concurrent packet workers should use disjoint physical cores across namespaces and endpoints. Record main/statistics thread placement as well.

Start receivers first. These are native endpoint command templates:

```sh
# Peer RX, host remote namespace.
sudo ip netns exec remote "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${NET_DEV[0]}" -a "${NET_DEV[1]}" -R "${NET_DEV[0]}" -R "${NET_DEV[1]}" \
  -l 0 -L 16-23 -s "${NET_IP[0]}" -s "${NET_IP[1]}" \
  -d "${ARM_IP[0]}" -d "${ARM_IP[1]}" \
  --rx-ip "${NET_IP[0]}" --rx-ip "${NET_IP[1]}" \
  -p 3333,3334,0 -M 65536 -S -f cross-net-rx

# Host RX, default namespace.
sudo "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${HOST_DEV[0]}" -a "${HOST_DEV[1]}" -R "${HOST_DEV[0]}" -R "${HOST_DEV[1]}" \
  -l 0 -L 16-23 -s "${HOST_IP[0]}" -s "${HOST_IP[1]}" \
  -d "${NET_IP[0]}" -d "${NET_IP[1]}" \
  --rx-ip "${HOST_IP[0]}" --rx-ip "${HOST_IP[1]}" \
  -p 3333,3334,0 -M 65536 -S -f cross-host-rx

# ARM RX, SoC.
sudo "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${ARM_DEV[0]}" -a "${ARM_DEV[1]}" -R "${ARM_DEV[0]}" -R "${ARM_DEV[1]}" \
  -l 1 -L 4-11 -s "${ARM_IP[0]}" -s "${ARM_IP[1]}" \
  -d "${NET_IP[0]}" -d "${NET_IP[1]}" \
  --rx-ip "${ARM_IP[0]}" --rx-ip "${ARM_IP[1]}" \
  -p 3333,3334,0 -M 65536 -S -f cross-arm-rx
```

After consumers start, verify ARP resolution and the printed destination MAC for every sending port. If ARP fails, use `--dst-mac` with verified destination MACs in TX-device order. Verify forwarding and delivered RX as well: a correct static MAC alone does not prove that an eSwitch forwards the traffic to the intended endpoint.

TX commands follow this pattern, repeating devices and addresses in corresponding port order:

```sh
# Host→Net TX, host default namespace.
sudo "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${HOST_DEV[0]}" -s "${HOST_IP[0]}" -d "${NET_IP[0]}" \
  -a "${HOST_DEV[1]}" -s "${HOST_IP[1]}" -d "${NET_IP[1]}" \
  -l 1,2-5 --udp-src-port 12345 --udp-src-port-count 1 \
  -p 3334 -z 1472 -t 0 -M 65536 -S -f cross-host-net-tx

# Net→Host TX, host remote namespace.
sudo ip netns exec remote "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${NET_DEV[0]}" -s "${NET_IP[0]}" -d "${HOST_IP[0]}" \
  -a "${NET_DEV[1]}" -s "${NET_IP[1]}" -d "${HOST_IP[1]}" \
  -l 0,8-15 --udp-src-port 12345 --udp-src-port-count 1 \
  -p 3334 -z 1472 -t 0 -M 65536 -S -f cross-net-host-tx

# ARM→Net TX, SoC: eight workers, four per port.
sudo "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${ARM_DEV[0]}" -s "${ARM_IP[0]}" -d "${NET_IP[0]}" \
  -a "${ARM_DEV[1]}" -s "${ARM_IP[1]}" -d "${NET_IP[1]}" \
  -l 1,4-11 --udp-src-port 12345 --udp-src-port-count 1 \
  -p 3334 -z 1472 -t 0 -M 65536 -S -f cross-arm-net-tx
```

For Net→ARM, use the Net→Host TX template with destination addresses changed to `ARM_IP`, and start ARM RX. For the two host–ARM directions, use Comm Channel. Its **transport server runs on ARM and transport client runs on the host**; the host still selects the application direction and message size. Start ARM first.

Build on each endpoint in the selected native or container SDK environment:

```sh
pkg-config --modversion doca-comch
case "$(uname -m)" in
  aarch64|arm64) comch_c_args=-DDOCA_ARCH_DPU ;;
  *) comch_c_args= ;;
esac
meson setup secure-channel-tester/build-comch-compatible secure-channel-tester \
  --buildtype=release "-Dc_args=$comch_c_args"
meson compile -C secure-channel-tester/build-comch-compatible
ldd secure-channel-tester/build-comch-compatible/doca_secure_channel
```

Launch templates (omit `sudo` inside a root container):

```sh
# Host transport client: Host→ARM. Use -d recv instead for ARM→Host.
sudo "$REPO/secure-channel-tester/build-comch-compatible/doca_secure_channel" \
  -p 01:00.0 -s 65535 -d send -c
# SoC transport server, both directions; start FIRST. Replace verified local PF/representor PCI IDs.
sudo taskset -c 4-8 "$REPO/secure-channel-tester/build-comch-compatible/doca_secure_channel" \
  -p 03:00.0 -r 01:00.0
```

Measure the consumer: ARM for Host→ARM, host for ARM→Host. Record the actual consumer interval duration. Record clock offsets and timestamp endpoint output on a common collector: differing host/ARM wall clocks must not be used directly to select overlapping intervals. The network directions use 1472-byte UDP payloads; Comm Channel uses its own 65535-byte message size. These are different data paths, not UDP directions emulated through the network.

## DPA send sink and receive supply

A DPA send test always needs a peer sink, even in a “standalone” run. Start peer RX using the template above with port **3333**, a fresh file prefix, and preferably the single physical port corresponding to `DPA_DEVICE`. Start `run_dpa send MEMORY THREADS SLOTS` afterward. Measure peer received bytes as well as the DPA sender's posted-byte counter. Broadcast destination MACs are used by the current sender; confirm the packets are received at the intended peer.

A DPA receive test needs peer traffic at UDP port **3333**. Before accepting a run, verify successful ARP resolution and a valid destination MAC on every sending port. If ARP fails, use the KV generator’s repeatable `--dst-mac` option with verified destination MACs in TX-device order. Do not accept broadcast fallback as successful resolution. Use the unchanged `dpdk_kv_gen` source-port controls. For each TX worker, source ports are `base + queue_index × count + packet_index % count`; workers on the same physical port cover separate contiguous ranges. On a single PF with 256 DPA threads, eight TX workers and `--udp-src-port-count 32` cover all 256 low-byte steering buckets:

```sh
# Host peer namespace; start the DPA receiver first.
sudo ip netns exec remote "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${NET_DEV[0]}" -s "${NET_IP[0]}" -d "${ARM_IP[0]}" \
  -l 0,8-15 --udp-src-port 12345 --udp-src-port-count 32 \
  -R "${NET_DEV[0]}" -L 16-23 --rx-ip "${NET_IP[0]}" \
  -p 3333 -z 1472 -t 0 -M 65536 -S -f dpa-receive-supply
```

The 1472-byte payload remains 1472 bytes total: KV writes a random 48-bit key into its first six bytes and leaves the rest zero. It rotates that key every ten generated packets per TX worker. Receive throughput counts frame bytes, not aggregation completion. The generator resolves MACs through its existing ARP path; use `--dst-mac` for an explicit destination (`-D` is not supported). Verify actual ARP results and DPA PF steering before accepting a run. The two-port validation resolved the existing ARM addresses and delivered traffic to both physical-PF DPA receivers without an ARP source patch.

For a thread sweep, choose TX workers per PF `Q = min(4, threads_per_PF)` and source-port count `threads_per_PF / Q`; the specified thread counts are powers of two, so this is integral. At 128 threads per PF, four TX workers per PF and count 32 cover all 128 buckets. Reduce TX workers for fewer than four threads per PF, and record the resulting supplier capacity. Never silently assume a count applies independently across every worker.

Do not launch independent DPDK primary processes over the same PF for supply/sink and cross traffic. Use the original KV program's combined `-R`, `-L`, and `--rx-ip` operation. For DPA supply plus Host→Net competition, transmit UDP3333 and receive UDP3334 in one peer process. Set `-p 3333,3334,0`: zero port-2 TX probability still enables its independent native destination RX report. An RX-only KV process uses `-l 0 -L 16-23`, so main core 0 has zero TX workers and eight RX workers. It still needs one `-s` and `-d` value per attached port because the original TX-port list remains populated; startup can perform ARP, and the original common setup provides a dummy TX queue for ARP replies. Use this RX-only mode for the peer sink.

Native `[Dst Ports]` RX reports separate UDP3333 and UDP3334 in the same approximately one-second interval, aggregated across physical ports. `-S` additionally reports each physical port's total RX; those totals include both flows during contention and cannot be assigned to the DPA flow alone. Use `TOTAL TX`/per-port NIC TX rates for actual supplier throughput: the original optional destination-TX byte counters are incremented before `fill_mbuf_with_packet` sets packet length and can print zero or stale byte rates. Destination **RX** counters count the received mbuf length. These original RX destination counters are worker-owned ordinary counters read by the main reporter; retain the original reads rather than adding atomic instrumentation. Use `-M 65536` for adequate RX pool capacity. Preserve supplied TX, received rates, startup ARP results, and loss counters. Give host TX, peer TX, peer RX, and host RX disjoint physical worker-core sets in concurrent trials; unique file prefixes do not isolate device or CPU ownership.

## Run and measure each case

Start receivers before senders and wait for initialization to complete.
Measure the workload and competing traffic over the same interval, excluding
startup and shutdown. Keep measurement duration and workload parameters
consistent between standalone and concurrent runs; save the raw output and
report the averaging interval with the result.

For memory, use `--continuous` with identical loop counts and stride 1 in
matched runs. Report device-reported throughput separately from completed
accessed bytes divided by batch wall time. Stop senders before receivers.

DPDK and FlexIO packet rates label MiB/s as `MB/s`: divide by 128 to obtain Gib/s. Use `delta_bytes * 8 / (seconds * 2^30)` for counters. Frame counters include Ethernet headers, whereas application service payload has 1024 useful value bytes per request. Record which basis each result uses. The DPA sender counts posted sends; that is not proof of delivery. The DPA receiver counts received bytes. Primary memory throughput is the device-reported sum of individual thread rates, assuming a 1.8-GHz clock. Keep total completed accessed bytes divided by batch wall time separately labeled; the two measures are not interchangeable.

Capture hardware NIC counters before/after the whole trial and immediately around the common measurement window while workloads remain live. Timestamp counter acquisition and retain software queue counters over that window. Hardware snapshots can bracket a slightly wider interval, and physical-port counters can include PF/eSwitch traffic beyond a particular SF consumer. Flag nonzero drops/out-of-buffer/errors for ownership and timing review; clean software counters alone do not establish a loss-free hardware path.

Wait for every expected `RX worker core ... receiving on port ... queue ...` startup record before enabling its packet source. Original KV destination RX output is an interval rate, not an added cumulative `FLOW_COUNTERS` record. Printed `TOTAL RX` missed/no_mbuf fields are already interval deltas: sum complete loss intervals inside the measurement window rather than subtracting their first and last values. Preserve collector timestamps and the native report interval.

If a short memory trial contains no usable completed report, preserve the failure and repeat with a bounded longer window at unchanged threads, buffer size, stride and loops. Choose the extension from the observed batch cadence. Do not pad missing reports. For sub-second reports, consistently use one completed report per collector one-second bin in matched controls. Preserve the native CommChannel `[Interval]` label.

## 1. Standalone baselines

For read/write, run both memory locations with `run_dpa OP MEMORY 256`, explicit `MEM_BYTES` and `MEM_LOOPS`, and **stride 1**. For send/receive, the selected standalone topology is **two physical ports with 128 threads per instance, 256 total**, using the [two-port commands below](#two-physical-ports). Send has 384 slots per instance/1.5 MiB total; receive has 256 KiB per thread/64 MiB total. No competitor is active. Keep single-port and dual-port measurements separate. Verify every requested thread context starts and allocation succeeds; retain resource failures instead of silently changing the total thread count.

For both send and receive, sweep two-port total thread counts are **2, 4, 8, 16, 32, 64, 128, 256**, divided equally between instances. A one-thread point can be retained as a separately labeled single-port control. Repeat for DPA and ARM memory. Send stays at **768 total slots**, 384 per instance, even when thread count changes. Receive stays at **256 KiB per thread**, so total receive storage changes with the sweep. Keep packet size and receive source-port distribution fixed. Collect matching supply/sink capacity controls to identify generator or receiver limitations. Record global/PFC pause counters and drops at both endpoints: receiver backpressure can reduce an unpaced generator’s accepted TX rate. Interpret low supply rate together with matching capacity controls and pause counters.

### Two physical ports

A single DPA instance on `mlx5_0` uses one 200-Gb/s link. Listening on both peer ports does not make that instance use the second link. To measure aggregate network throughput, launch one existing DPA program on each physical PF (`mlx5_0` and `mlx5_1`) concurrently. The theoretical one-link ceiling is 186.26 Gib/s before framing overhead.

Record thread and buffer counts **per instance and in total**. For a controlled 256-thread comparison, use **128 threads per port**; send uses **384 slots per instance**, giving 768 slots/1.5 MiB total. Receive retains 256 KiB per thread, giving 32 MiB per instance/64 MiB total.

For send, start one dual-port peer receiver first:

```sh
sudo ip netns exec remote stdbuf -oL -eL "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${NET_DEV[0]}" -a "${NET_DEV[1]}" -R "${NET_DEV[0]}" -R "${NET_DEV[1]}" \
  -l 0 -L 16-23 -s "${NET_IP[0]}" -s "${NET_IP[1]}" \
  -d "${ARM_IP[0]}" -d "${ARM_IP[1]}" \
  --rx-ip "${NET_IP[0]}" --rx-ip "${NET_IP[1]}" \
  -p 3333,3334,0 -M 65536 -S -f dual-dpa-send-sink
```

Use two SoC terminals, setting `PF_INDEX=0` in the first and `PF_INDEX=1` in the second. In each terminal define the following and launch the appropriate send or receive command. Add `-a` to both instances through `PORT_MEMORY_ARGS` for ARM packet storage:

```sh
PF_INDEX=0 # Set 1 in the second SoC terminal.
DPA_DEV=(mlx5_0 mlx5_1) # Verify each physical PF mapping.
PORT_THREADS=128
PORT_SEND_SLOTS=384
PORT_MEMORY_ARGS=()
[ "$MEM" = arm ] && PORT_MEMORY_ARGS=(-a)

# Send: run concurrently on both PFs after the peer sink is ready.
sudo stdbuf -oL -eL "$REPO/dpa-all-reduce/sender/build-doca31/host/flexio_packet_processor" \
  "${DPA_DEV[$PF_INDEX]}" -t "$PORT_THREADS" -k "$PORT_SEND_SLOTS" \
  -z 1472 -p 3333 -i "${NET_IP[$PF_INDEX]}" -m "$DPA_MAC" "${PORT_MEMORY_ARGS[@]}"

# Receive: run concurrently on both PFs before starting the peer supplier.
sudo stdbuf -oL -eL "$REPO/dpa-all-reduce/receiver/build-doca31/host/flexio_packet_processor" \
  "${DPA_DEV[$PF_INDEX]}" -t "$PORT_THREADS" -p 3333 "${PORT_MEMORY_ARGS[@]}"
```

For receive, launch one original KV supplier after both DPA receivers are ready. At 128 threads per PF, use four TX workers/queues per PF and 32 source ports per queue. Core 0 is the DPDK main thread; eight peer RX workers on 16–23 handle the existing ARP and cross-traffic path:

```sh
PORT_THREADS=128
TX_WORKERS_PER_PF=$(( PORT_THREADS < 4 ? PORT_THREADS : 4 ))
SOURCE_PORTS_PER_QUEUE=$(( PORT_THREADS / TX_WORKERS_PER_PF ))
LAST_TX_CORE=$(( 7 + 2 * TX_WORKERS_PER_PF ))
sudo ip netns exec remote stdbuf -oL -eL "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${NET_DEV[0]}" -a "${NET_DEV[1]}" -l "0,8-${LAST_TX_CORE}" \
  -s "${NET_IP[0]}" -s "${NET_IP[1]}" -d "${ARM_IP[0]}" -d "${ARM_IP[1]}" \
  --udp-src-port 12345 --udp-src-port-count "$SOURCE_PORTS_PER_QUEUE" \
  -R "${NET_DEV[0]}" -R "${NET_DEV[1]}" -L 16-23 \
  --rx-ip "${NET_IP[0]}" --rx-ip "${NET_IP[1]}" \
  -p 3333,3334,0 -z 1472 -t 0 -M 65536 -S -f dual-dpa-receive-supply
```

Apply the same initialization and measurement procedure above. Collect per-port RX and aggregate RX over the same fixed common live window. Send can use common peer report intervals; receive counter intervals must retain their timestamps and any alignment offset. Report the paired aggregate mean and each port's mean; retain interval alignment rather than summing independently chosen maxima. Retain actual supplied TX, PAUSE and drop counters to identify supply or service limitations.

Use identical physical ports, instance count, aggregate threads and buffer storage for each workload and its matching control. Do not normalize measurements from different topologies against each other.

## 2. Contention matrix

Use the same eight 256-thread configurations and buffer settings as Experiment 1. Pair read/write/send with all six directions. Pair receive only with **Host→Net, ARM→Net, Host→ARM, ARM→Host**. First measure the six flows alone using the endpoint recipes, then measure the **44** valid concurrent cases. Preserve the supplying DPA network workload for every send/receive control and concurrent case.

Report both DPA rate and competing consumer rate, plus reductions relative to their matching standalone baselines. A stalled competing consumer is not a valid offered-load control. Verify the DPA and cross UDP rules select different destination ports, and retain forwarding and steering configuration.

If a receive workload nearly starves a competing flow, first verify source readiness, ARP and steering. Retain accepted TX, delivered RX and timestamped pause/drop counters. Backpressure can suppress accepted TX; distinguish that observation from a failed source. Do not infer exact packet loss from independently timed native rates.

## 3. Send-buffer-size sweep

Use **16 DPA threads total, eight per physical port**, both DPA and ARM send storage, standalone and against each of the six flows. Change only `sender -k`; do not change packet size, SQ depth, threads, or competing topology.

Send-buffer grid:

| Total data storage | Bytes | Sender `-k` slots |
| --- | ---: | ---: |
| 1.5 MiB | 1,572,864 | 768 |
| 2 MiB | 2,097,152 | 1024 |
| 3 MiB | 3,145,728 | 1536 |
| 4 MiB | 4,194,304 | 2048 |
| 8 MiB | 8,388,608 | 4096 |
| 12 MiB | 12,582,912 | 6144 |
| 16 MiB | 16,777,216 | 8192 |

These slot counts are aggregate across both instances. Split a total `S` into `S/2` rounded down on PF0 and the remaining slots on PF1. For example, exact 3 MiB total uses eight threads and `-k 768` on each PF. Retain printed allocation sizes. At seven sizes this grid requires **98 configurations** (`7 × 2 × 7`), before repetitions; shared identical standalone controls need not be rerun for every direction. These points specify allocated storage, not on-wire payload bytes or an assumed cache capacity. Validate every pool is fully initialized, including non-divisible slot/thread counts.

## 4. DPA-thread and host-TX-worker/queue sweeps

Use DPA-memory send storage only, with **768 slots (1.5 MiB)** and **4096 slots (8 MiB)**, aggregate across the two DPA instances. Host→Net cross traffic uses the **unchanged multithreaded `dpdk_kv_gen`**. Each TX worker owns one queue; workers are assigned alternately to the two physical ports. With `Q` workers per PF, allocate `2 × Q` TX workers and verify startup reports `Q` TX queues per PF. The DPDK main core is additional and does not send traffic.

```sh
# Host default namespace; start the peer consumer first.
HOST_TX_WORKERS_PER_PF=2  # Fixed setting, four TX workers total.
LAST_HOST_TX_CORE=$(( 1 + 2 * HOST_TX_WORKERS_PER_PF ))
sudo "$REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "${HOST_DEV[0]}" -s "${HOST_IP[0]}" -d "${NET_IP[0]}" \
  -a "${HOST_DEV[1]}" -s "${HOST_IP[1]}" -d "${NET_IP[1]}" \
  -l "1,2-$LAST_HOST_TX_CORE" --udp-src-port 12345 --udp-src-port-count 1 \
  -p 3334 -z 1472 -t 0 -M 65536 -S -f cross-host-net-kv
```

(a) Fix host workers/queues per port at two and sweep total DPA threads **2–256 in powers of two**, half per PF, at each aggregate send-buffer size. Use the same fixed host-worker count in every matched control. Keep an optional one-thread single-port control separately identified. Measure DPA-only controls at every thread/size pair and a matching Host→Net-only control.

(b) Fix total DPA threads at **16** (eight per PF) and sweep **1, 2, 4, 8, 16 and 31 total DPDK TX workers**, excluding the main core. These are aggregate worker counts across both physical ports, not counts per PF. The original generator assigns workers alternately to the ports; odd counts therefore produce unequal port allocations, and one worker exercises only one competing port. Retain each port separately and label this geometry. For each point, set the exact total TX worker count and update the TX lcore set, and collect a Host→Net-only control plus matching DPA-only controls at both send-buffer sizes. Verify actual workers and queues in startup logs. Use the original KV RX-only peer with `-p 3333,3334,0` to measure DPA output and competing traffic separately.

This is a **worker/queue sweep**: CPU parallelism and queue count increase together, as in the original KV toolkit. It is not a queue-only sweep with a fixed worker count. Do not substitute a one-worker multi-queue generator or add source-port cycling to hide the original queue-dependent packet distribution. Keep `--udp-src-port-count 1` fixed; source-port diversity then naturally changes with the number of workers/queues.

The example host TX cores 2–5 and peer RX cores 16–23 are disjoint physical cores only after verifying the local CPU topology. Reassign them for larger worker counts. If a point cannot fit disjoint workers plus main/reporting cores, record it as resource-limited or use a disclosed smaller peer pool with capacity checks and matched standalone controls; do not silently place workers on SMT siblings. Never reduce the peer sink without checking that it can receive the intended combined load.

For example, a 24-physical-core host with eight peer RX workers and two main cores has room for at most fourteen host TX workers, or seven per PF. Eight TX workers per PF needs a smaller validated peer sink or a different machine. If peer supply also needs eight TX workers, only six physical host TX worker cores remain, or three per PF. These are core-budget checks, not universal queue limits.

Record total/per-PF workers and queues, core assignments, source-port coverage and actual competing RX. The host-worker sweep fixes 16 total DPA threads and uses both required buffer sizes.

## 5. Hierarchical parameter server

Interpret workload fraction as the share of **input aggregation work** assigned to DPA, not a fraction of measured output throughput or a sampling/drop fraction. Use fractions **0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100%** for each DPA packet-buffer location. ARM service packet storage remains ARM memory; DPA `-a` switches its packet storage, while its aggregation table remains DPA-resident.

Build the repository revision being reproduced. The following optional command creates an isolated checkout:

```sh
HIERARCHY_REPO=$(mktemp -d)
git archive HEAD | tar -xf - -C "$HIERARCHY_REPO"
```

Set `HIERARCHY_REPO="$REPO"` when already using a clean original checkout. Record the resolved revision and source hashes. In the original hierarchy tree, build `all-reduce-app`, its `dpdk_mapreduce` subproject, and the `dpdk_kv_gen` target using the native Meson commands above. Use the corresponding architecture on each machine; mount this tree into Docker if using the optional container shells. This selects repository code; apply only documented compatibility fixes required by the chosen revision.

Use the existing **`packet_gen_recvs/build-dpa/dpdk_kv_gen`** and original hierarchy service sources. Build the KV generator, its common dependencies and both hierarchy services from the same revision. Preserve source identities. Do not substitute `dpdk_simple_gen`, a custom hierarchy client, or a zero-key payload. This generator writes a random **48-bit key into the first six payload bytes** and rotates it every ten generated packets per sender worker. The following 128 uint64 values are zero. Its existing `-p 1234,1235,P` selects port 1235 with probability `P` independently per packet, so use `P = 1 − DPA_fraction/100`. This preserves the original packet dispatch behavior; it does not prove completed-group correctness. Record the realized endpoint RX split.

A concrete branch layout to validate is DPA destination port **1234**, ARM destination port **1235**, same aggregation threshold **10**, and identical fixed-size requests. Verify disjoint hardware steering and concurrent PF ownership before measurement; first verify the existing DPA and ARM UDP steering rules on the same PF. `dpdk_kv_gen` uses one destination MAC per TX device, resolved through ARP or set with `--dst-mac`; it has no per-branch destination-MAC option. A separate SF branch cannot be assumed reachable through that same MAC. Do not assume two independently created steering domains coexist correctly. The dispatcher must preserve branch routing, source-port buckets, reply association, and saturation load on that topology. Exclude client-side starvation or steering failures with branch-only controls.

Native branch launch templates after verifying UDP steering. Use the physical PF shared with DPA for this ARM service, rather than the SF devices assigned to cross traffic above:

```sh
HIERARCHY_ARM_DEVICE=03:00.0 # Example SoC PF: verify its mapping to DPA_DEVICE.
HIERARCHY_ARM_IP=${ARM_IP[0]}

# SoC DPA service: omit -a for DPA packet storage, add it for ARM packet storage.
sudo stdbuf -oL -eL "$HIERARCHY_REPO/dpa-all-reduce/all-reduce-app/build-doca31/host/flexio_packet_processor" \
  "$DPA_DEVICE" -t 256 -p 1234 -k 10

# SoC ARM service: SIXTEEN workers, INCLUDING the main lcore.
sudo stdbuf -oL -eL "$HIERARCHY_REPO/dpa-all-reduce/all-reduce-app/dpdk_mapreduce/build-doca31/dpdk_mapreduce" \
  -a "$HIERARCHY_ARM_DEVICE" -l 0-15 -p 1235 --rx-ip "$HIERARCHY_ARM_IP" --limit 10 \
  -f hierarchy-arm-service -S
```

Use **16 ARM service workers**. This application includes the main lcore in packet processing: `-l 0-15` supplies 16 workers on one port, rather than 15 workers plus an idle main core. Verify `Total workers: 16` and every queue assignment in startup output. On a 16-core SoC this occupies all ARM cores, so the DPA control process and client-management tasks share scheduler time; retain that placement in the controls. If worker resources are insufficient, report the limitation; changing worker count defines a different configuration and requires matching controls. At 0%, instantiate only the ARM service; at 100%, instantiate only the DPA service. Keep the unused endpoint off for these single-endpoint controls and reuse controls only when worker placement and telemetry match. At intermediate fractions instantiate both services concurrently.

The reported hierarchical throughput is the **highest sustained aggregate RX immediately before either active endpoint enters confirmed overload**. For configured DPA fraction `f`, report ARM RX, DPA RX, their sum, actual sender TX, configured fraction and realized DPA RX share. The first endpoint to overload limits this fraction; at 0% or 100% only the active endpoint matters. Do not select an aggregate peak after one endpoint has entered overload.

**Overload definition:** increasing an endpoint's offered load produces a repeatable plateau or decrease in its received throughput beyond observed run-to-run variability, accompanied by increasing native receive misses or delivery deficit. Nonzero drops alone do not establish overload. Repeat the points on both sides of the suspected transition to confirm it. Use endpoint-specific accepted TX if trustworthy counters are available. Otherwise estimate endpoint offered load as `f × sender TX` for DPA and `(1 − f) × sender TX` for ARM, explicitly recording that the original generator's configured random dispatch split is an estimate rather than a measured accepted per-destination count. Its pre-fill destination byte counters are unsuitable for this purpose. Independently timed TX/RX differences are delivery-deficit estimates, not certified packet loss. DPA exposes no application drop counter, so increasing estimated endpoint deficit can support its RX plateau/decline; retain counter scope and uncertainty. Received-fraction drift supports identifying which endpoint is failing, but is not an independent arbitrary numerical cutoff.

Use an adaptive bracket-and-refine search for all fractions **0–100% in 10% steps**, with both DPA and ARM packet-buffer locations:

1. First validate standalone ARM capacity and DPA capacity for each buffer location with the same source, workers and telemetry. Measure these controls afresh within the new reproduction series. Calibrate the pause-to-TX relationship and capacity boundary within the current measurement series. Let observed pre-overload capacities be `C_ARM` and `C_DPA`.
2. Estimate the initial total-load boundary as `min(C_ARM/(1−f), C_DPA/f)`, omitting the inactive endpoint's term at 0%/100%. This estimate guides the search; it does not prove mixed-workload capacity. Use only the sender pause-to-TX relationship measured in this fresh series to choose a pause count; pause counts are not promised rates.
3. For fresh standalone calibration, use a predeclared cold-start pause of 12800, then geometrically halve it (6400, 3200, 1600, 800, 400, 200, 100, then 0) as needed to discover underload and overload; if 12800 is already overloaded, double it until responsive. This protocol is fixed independently of previous measurements. Start mixed fractions below the estimate obtained from these fresh controls and increase offered traffic by reducing the fixed pause between short trials. If the starting point is overloaded, increase pause until a responsive point is found. Establish a bracket consisting of a responsive lower-load point and a suspected overloaded higher-load point. Inspect ARM and DPA separately. If sender capacity prevents reaching overload, label the result source-limited/unbracketed.
4. Refine only inside that bracket using integer midpoint pause counts, or interpolate using measured TX to target the midpoint offered load. Sender pause versus load is nonlinear; inspect actual TX after each trial. Binary search locates the transition only within a verified local bracket, not an assumed globally monotonic throughput curve. Keep every observation. If intermediate behavior is contradictory, repeat it and split the region rather than forcing binary classification.
5. Repeat both boundary points with unchanged commands. Judge plateau/decline against their observed variability and rising drop/deficit evidence. Noise alone is a flag, not a reason to discard a result or extend a trial. Continue local refinement until the bracket's offered-load width is comparable to measured repeatability or finer refinement no longer changes classification. Record the achieved resolution and stop reason; if no clear transition is confirmed, report an unresolved bracket.
6. Select the highest sustained aggregate RX on the confirmed pre-overload side, reporting all repetitions, limiting endpoint, lower/upper offered loads, throughput ranges, endpoint deficits/misses and configured/realized fractions. Retain overload points with the selected boundary.

Keep `dpdk_kv_gen` traffic, random keys, packet size, source-port geometry,
and worker placement fixed. Keep `-t` constant within each measurement and
adjust it between trials. Compare endpoint RX and sender TX over the same
interval, retaining loss counters alongside throughput.

Use one peer physical port and record its negotiated speed. In the measured setup each physical link is **200 Gb/s**; verify this instead of assuming 100 Gb/s. Keep sender worker placement fixed throughout the curve after recording actual sender TX and worker controls.

The following explicit settings define the hierarchy workload. CPU assignments are examples to adapt to the available physical cores:

```sh
# Set these in the peer terminal; use Bash for the endpoint arrays.
PEER_DEVICE=${NET_DEV[0]}
PEER_IP=${NET_IP[0]}
HIERARCHY_ARM_IP=${ARM_IP[0]}
PEER_TX_LCORES=8-15
PEER_RX_LCORES=0,16-23   # Main core 0 is filtered out; actual reply workers are 16-23.
SOURCE_PORT_BASE=1
SOURCE_PORT_COUNT=65535 # Exercises the 16-bit DPA steering/index space; note worker wrap.
# Example 30% DPA routing check; tune PAUSE_COUNT before measuring saturation.
ARM_FRACTION=0.7
PAUSE_COUNT=6400
RUN_ID=hierarchy-dpa-f30-p6400-r1 # Use a unique prefix for every trial.
```

Generator launch template (omit `ip netns exec remote` on a separate peer host):

```sh
sudo ip netns exec remote stdbuf -oL -eL "$HIERARCHY_REPO/packet_gen_recvs/build-dpa/dpdk_kv_gen" \
  -a "$PEER_DEVICE" -l "$PEER_TX_LCORES" \
  -s "$PEER_IP" -d "$HIERARCHY_ARM_IP" -p "1234,1235,$ARM_FRACTION" \
  -z 1030 --udp-src-port "$SOURCE_PORT_BASE" --udp-src-port-count "$SOURCE_PORT_COUNT" \
  -t "$PAUSE_COUNT" -R "$PEER_DEVICE" -L "$PEER_RX_LCORES" \
  --rx-ip "$PEER_IP" -M 65536 -S -f "$RUN_ID"
```

Use the explicit source-port base/count and sender worker placement below consistently across the hierarchy curve. The defaults are base 12345 and one source port per sender queue; the explicit template uses base 1 and count 65535. Source-port diversity affects ARM queue assignment and aggregation, so a 16-bit table capacity does not by itself determine the correct generator setting.

Set `ARM_FRACTION` to 0.0 through 1.0 for DPA fractions 100% through 0%. Use only port 1234 for DPA-only controls and only port 1235 for ARM-only controls. Drain replies using the existing receiver or the generator's existing `-R`/`-L` options on separate peer cores; verify the actual worker assignments before measurement. The advertised `--flow-count` option has no implemented argument handler and must not be used.

The payload is six random key bytes plus **128 little-endian uint64 values**: 1030 UDP bytes and a 1072-byte Ethernet frame excluding FCS. Preserve the generator's existing key rotation, payload and pacing logic. Do not replace random keys with zero padding. Record source-port base/count and sender queue IDs: the existing uint16 source-port expression wraps for later workers when the count is 65535, so multiple workers collectively may include port zero. That is separate from the random 48-bit payload-key space.

The primary metric is **service endpoint RX throughput**, measured independently from the DPA and ARM approximately one-second reports. Sum their selected RX rates for aggregate service RX; do not add TX or client reply RX. Retain client/request/reply validation only as ancillary evidence. Printed endpoint `MB/s` is MiB/s; divide by 128 for Gib/s. Align the selected service intervals on the collector clock and retain actual interval boundaries.

The original Git version of `all-reduce-app/dev/flexio_packet_processor_dev.c` computes `idx = sport >> app_ctx->sport_shift` and starts aggregation after `AGGREGATION_PAD_BYTES`: DPA skips the six random key bytes. At 256 DPA threads, the lower eight source-port bits select a thread and the upper eight bits index its aggregation state: a 16-bit source-port space with 65,536 possible values, independent of the sender’s configured count. On ARM, the original `dpdk_mapreduce.c` hashes the random six-byte payload key together with the shifted source port into a 16-bit table index. Zeroing these six bytes changes ARM's memory-access workload and invalidates comparison with `dpdk_kv_gen` results. 

Preserve the original service table/index/hash behavior. Retain exact binary/source identities and report RX-only measurements separately from completed-work validation.

## 6. Paced cross traffic with DPA reads

Use 256 DPA read threads and both memory locations, stride 1, with explicit read region size and short repeated batches as specified by the measurement protocol. The competitor is **Host→Net**. Sweep these exact fixed `rte_pause()` counts using the network generator's `-t` option:

| `-t` pause calls per loop |
| ---: |
| 1 |
| 100 |
| 200 |
| 300 |
| 400 |
| 800 |
| 1600 |
| 6400 |

Run every count with reads from **both DPA and ARM memory**, plus one no-cross-traffic baseline per memory location: 18 configurations before repetitions. Keep network worker/queue placement, devices, packet size, and read parameters fixed. For example, use the Host→Net TX template with `-t 100` for that point. Use the existing pause control and report measured rates for each fixed count. The independent variable is the fixed pause count, not a requested Gib/s value.

Report mean read throughput and the competing consumer's mean received rate
over the same interval. Plot read throughput against pause count and retain
achieved RX rate and losses alongside it. A no-cross-traffic baseline has no
competing sender; `-t 0` means an unpaused active generator.


## 7. DPA receive capped at 50 Gib/s

Use 256 DPA receiver threads total (128 per PF), 256 KiB receive storage per thread, and both memory locations. Repeat **Host→Net, ARM→Net, Host→ARM, ARM→Host**, plus two no-competitor capped controls. Preserve all 128 receive-thread source-port buckets on each PF, 256 buckets across the two ports, while pacing the **DPA receive supply**, independently of the competitor. Configure competing traffic as in its matching standalone control and retain actual concurrent TX/RX; backpressure can reduce delivered load.

The receiver has no `--rate` or throttle flag. Cap by pacing its peer supply and verifying the DPA receiver's actual byte counter is approximately **50 Gib/s** over the measured window. Use the existing KV `-t` count of `rte_pause()` calls to approach this rate; it is an approximate offered-rate setting, not a strict cap. Report the achieved receive rate. With 1472-byte UDP payloads, 1514-byte Ethernet frames excluding FCS, and the receiver's frame-byte accounting, 50 Gib/s corresponds to approximately **4,432,554 frames/s**, aggregate across whichever ports the DPA workload uses. Do not interpret the target as decimal 50 Gbps or include cross-flow bytes in it.

First calibrate standalone supply using short trials with a fixed `-t` pause count in each trial; adjust the count only between trials until supplied TX is near 50 Gib/s. Preserve that count in matching concurrent runs and report actual supplier TX, DPA RX, drops, and competing RX. If contention lowers DPA RX below target, retain the under-target value. Keep pacing fixed within each trial and preserve source-port coverage.

Retain the uncapped receive control. If its RX ceiling is already below or near the target, label that capacity limit even when a nonzero pause count is used: the pacing command alone does not establish that pacing imposed the observed RX ceiling. Interpret actual supplier TX and PAUSE counters alongside the receiver rate.

## Implementation checks before measurement

These checks concern measurement validity and the controls available in the recorded source revision.

- **256-thread startup:** verify hardware contexts, memory registrations, and full startup at 256 threads with each required placement before accepting that trial. For receiver, verify configured source-port ranges cover all intended steering buckets and retain every TIR startup record. The original receiver exposes aggregate rates, so these checks establish configuration coverage rather than balanced measured traffic in every bucket. For sender, verify initialized shared storage and received output.
- **Sender checksums:** `sender/dev/flexio_packet_processor_dev.c` calls `flexio_dev_swqe_seg_eth_set(swqe, 1 << 14, 0, 0, NULL)`. The FlexIO SDK defines bit 14 (`0x4000`) as `FLEXIO_ETH_SEG_L3CS`, requesting hardware IPv4-header checksum offload. The L4/UDP checksum-offload bit is not enabled; the UDP checksum remains zero. Record the offload setting and verify the emitted IPv4 checksum in captured packets. The requested offload alone does not establish that the emitted checksum is valid.
- **Memory batch accounting:** the supplied host loop enqueues each task once per batch and reads completed results before submitting the next batch. Verify these properties when comparing another revision. Primary memory throughput is the device-reported sum at the assumed 1.8-GHz clock; wall-time throughput is a separate diagnostic.
- **Receiver reporting:** use the original RX rate output and external collector timestamps. Verify thread setup and source-port coverage through existing startup records, steering rules and generator configuration; disclose limits of the available measurements.
- **Original receive supply and pacing:** use KV `--udp-src-port`/`--udp-src-port-count` with its queue-index source-port ranges, native two-destination RX rates, and fixed `-t` pause counts. Verify actual worker/queue allocation, source-port steering coverage, ARP resolution, supplier/receiver capacity, RX pool size, and losses.
- **Hierarchical routing:** use unchanged `dpdk_kv_gen`, preserve its random 48-bit payload keys and existing fraction/throttle options, verify source-port steering coverage and measured steady service RX loss. Find the capacity boundary with fixed-rate trials and retain both overload observations and small-loss operating candidates. Keep 16 ARM workers. Keep primary service RX counters separate from client useful-work diagnostics.
- **Original hierarchy sources:** run the repository service and `dpdk_kv_gen` sources without custom counters, RPCs, reply fixes, or a replacement generator. Retain source and binary hashes. Original logs provide RX throughput. Verify queue assignment using existing startup records and source-port steering settings.

## Recording results

Retain a machine-readable row per repetition with: experiment/run ID, revision, operation, placement, threads, total/per-thread buffer bytes, stride/loop count, ports, queues/workers, direction, offered and realized pacing, workload fraction, measurement start/end, DPA/ARM/aggregate throughput, competing received throughput, losses, validation status, and both ASIC temperatures. Save exact command lines and raw endpoint logs beside it, plus software/driver/firmware versions and container digests where used.

Count matrix configurations separately from repetitions and reused controls. Do not conflate received frame throughput with useful service throughput. Report all seven experiment families, the 44-case contention matrix, the selected buffer-size sweep including exact 3 MiB, and the eight capped-receive concurrent cases. Include failures with reasons and do not average incomplete intervals.

Retain the repository revision, source/binary hashes, exact commands, environment versions and device/queue mappings with the result files. Shut down only processes and containers created for the experiment.


## ARP and explicit destination MAC

The KV generator accepts repeatable `--dst-mac XX:XX:XX:XX:XX:XX` values in TX-device order. Supply one verified unicast MAC per TX device when ARP fails; without this option the ARP path is used. Use the MAC of the intended ingress endpoint: physical-PF DPA ingress and an ARM SF can route differently. Verify both per-port startup identities and delivered RX. The DPA sender’s `-m` sets its source MAC; its destination remains broadcast.
