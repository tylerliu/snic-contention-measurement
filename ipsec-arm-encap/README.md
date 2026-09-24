# Discussion experiment: ARM ESP decapsulation and monitoring

This directory contains the Discussion experiment for “The Hidden Cost of
Heterogeneity: Navigating Internal Contention in SoC SmartNICs.” Hardware
handles ESP authentication/decryption; ARM DPDK workers decapsulate packets
and enforce a 256-packet per-SA replay window. DOCA can sample/clone packets
toward monitor SF interfaces while clear traffic is forwarded to the host.

## Key result and Scenario 3 comparison

**Four ARM workers sustain over 180 Gib/s with cloning, IPsec ESP decapsulation,
and anti-replay enabled**, using ARM decapsulation/anti-replay to bypass the NIC
pipeline feature-composition penalty identified in Scenario 3. ESP
authentication/decryption remains in hardware.

The supporting retained run is `decap-isolate-sweep_sf1-1550-sfcompare1g-static`,
using `decrypt_16tx_4arm_sf1.yml`: four ARM workers, sampling fraction 1.0,
16 TX workers, eight SAs per port, and 1414-byte UDP payloads. Its clear-host RX
rate is **183.011821 Gib/s**, averaged over samples 8–23. It reports 9,344
whole-trial SoC RX misses and zero software TX/replay drops and host
missed/no-mbuf events. The claim concerns delivered throughput; it does not
claim lossless delivery or establish clone contents from pipe counters alone.

The Scenario 3 result is **87.59046875 Gib/s**. In that scenario,
enabling IPsec ESP anti-replay increases throughput loss to 69.0%
without sampling and to 70.2% as sampling increases. The ARM operating point
is **2.09×** the Scenario 3 rate. Both experiments use a **1500-byte link MTU**, a **dual-port DPDK
application without link aggregation**, the **same hardware setup**, and the
**same receiver**. The link MTU is distinct from the UDP payload and frame
lengths used by the analysis script below; it is not a 1500-byte UDP payload.

To reproduce the retained cloning-enabled operating point after hardware setup,
run from this directory with a fresh label:

```sh
bash benchmarks/run_decrypt_isolation.sh sweep_sf1 1550 cloning180a 16 static
bash benchmarks/summarize_decrypt_sweep.sh sweep_sf1
```

Inspect the `cloning180a` row and retain its TX/RX/SoC logs. The throughput
criterion for this result is clear-host RX greater than 180 Gib/s with
four ARM workers, sampling fraction 1.0, and anti-replay enabled. Verify the
configuration and monitoring counters as described below.

## Included materials

- `monitoring_*.cpp`, `device_manager.*`, `arg_parser/`: application and configuration.
- `decrypt_16tx_1arm.yml`, `decrypt_16tx_4arm.yml`: one/four-worker baselines.
- `decrypt_16tx_4arm_sf0.yml`, `decrypt_16tx_4arm_sf1.yml`: monitor destinations
  enabled with sampling fraction 0.0/1.0.
- `esp_layout_test.cpp`, `esp_replay_test.cpp`: layout and replay-window tests.
- [Run procedure](benchmarks/RUN_ARM_ANTI_REPLAY_SAMPLING_CLONING.md): historical
  lab commands, pacing sweeps, and counter checks.
- `benchmarks/decap-isolate-*-{tx,rx,soc}.log`: retained raw run triplets.
- `benchmarks/summarize_decrypt_sweep.sh`: CSV extraction from those logs.

Other benchmark reports describe development and earlier experiments. Their
paths, source revisions, and result availability must be checked before reuse;
not every historical sweep's raw logs are included.

## Analyze existing results (no special hardware)

From the repository root:

```sh
cd ipsec-arm-encap
bash benchmarks/summarize_decrypt_sweep.sh sweep1
bash benchmarks/summarize_decrypt_sweep.sh sweep
bash benchmarks/summarize_decrypt_sweep.sh sweep_sf0
bash benchmarks/summarize_decrypt_sweep.sh sweep_sf1
```

Requirements are Bash and awk. Output is CSV with one row per retained triplet.
Rates average interval samples 8–23 separately for TX and RX. The script assumes
1510-byte encrypted frames and 1456-byte clear frames (1414-byte UDP payload),
and reports Gib/s using 2³⁰ bits/s. It is not a general packet-size analyzer.
Miss counters cover the whole trial rather than that rate averaging interval.

| Retained operating point | Clear host RX (Gib/s) | Interpretation |
| --- | ---: | --- |
| One worker, pacing 7200, two runs | 72.431353 mean | Peak tested delivery; SoC RX misses occur |
| One worker, pacing 7800 | 67.938572 | Zero reported SoC RX misses |
| Four workers, monitoring disabled, pacing 1400, two runs | 189.735 mean | Zero reported SoC RX misses |
| Four workers, SF fraction 0.0, pacing 1500 | 185.297 | SoC RX misses occur |
| Four workers, SF fraction 1.0, pacing 1550 | 183.012 | SoC RX misses occur |

Sources: [one-worker retune](benchmarks/SA_STEERING_SINGLE_WORKER_RETUNE.md),
[four-worker retune](benchmarks/SA_STEERING_RETUNE.md), and
[SF comparison](benchmarks/ARM_ANTI_REPLAY_SF_COMPARISON.md).
These are recorded operating points, not acceptance thresholds. The two SF
runs differ in pacing, so their difference does not isolate cloning overhead.
Monitor-output pipe counters alone do not verify cloned contents or SF delivery.

## Build and component checks

The application requires Linux, C/C++ compilers, Meson/Ninja, DPDK,
`doca-common`, `doca-flow`, `doca-dpdk-bridge`, libyaml (`yaml-0.1`), and pthreads.
Use the DOCA 3.1 DPU environment from the run procedure, with its pkg-config
paths configured. From the repository root of a complete checkout on the DPU:

```sh
meson setup ipsec-arm-encap/build ipsec-arm-encap
meson compile -C ipsec-arm-encap/build
meson test -C ipsec-arm-encap/build --print-errorlogs
```

### Build the host generator and receiver

The runner uses these targets from [packet_gen_recvs](../packet_gen_recvs/):

| Role | Source | Executable |
| --- | --- | --- |
| ESP TX, one SA per worker | `dpdk_esp_exclusive_gen.c` + `dpdk_common.c` | `packet_gen_recvs/build/dpdk_esp_exclusive_gen` |
| Clear-packet host RX | `dpdk_simple_recv.c` + `dpdk_common.c` | `packet_gen_recvs/build/dpdk_simple_recv` |

Build both in the DOCA 3.1 host environment. From the repository root, for a
fresh build directory:

```sh
sudo docker run --rm \
  -v "$PWD/packet_gen_recvs:/workspace" -w /workspace \
  nvcr.io/nvidia/doca/doca:3.1.0-devel-host \
  sh -lc 'meson setup build -Dpkg_config_path=/opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig && meson compile -C build dpdk_esp_exclusive_gen dpdk_simple_recv'
```

For subsequent builds in the same environment, replace the `sh -lc` command
with `meson compile -C build dpdk_esp_exclusive_gen dpdk_simple_recv`.
The runner resolves `packet_gen_recvs` relative to its own location, checks
that both executables exist, and mounts that directory at `/workspace` in
both host containers. Inside the containers the paths are
`./build/dpdk_esp_exclusive_gen` and `./build/dpdk_simple_recv`.

The ESP runner uses `-d` for inner destination IP, `-T` for tunnel destination,
`--dst-mac` for static Ethernet destinations, and `-t` for pacing. With
`-l 0-16`, one main lcore and 16 TX workers are selected; the exclusive
generator automatically assigns eight SAs per port across the two ports.
`dpdk_esp_gen` is the separate shared-SA generator and is not used by this run.

### Component checks without hardware

On a machine without DOCA/DPDK, run the two header-only component tests with
a C++11 compiler. From the repository root:

```sh
check_dir=$(mktemp -d)
c++ -std=c++11 -Wall -Wextra ipsec-arm-encap/esp_layout_test.cpp -o "$check_dir/esp_layout_test"
c++ -std=c++11 -Wall -Wextra ipsec-arm-encap/esp_replay_test.cpp -o "$check_dir/esp_replay_test"
"$check_dir/esp_layout_test" && "$check_dir/esp_replay_test"
```

Both exit successfully without output when their assertions pass. Keep
assertions enabled (do not add `-DNDEBUG`). These test layout parsing and replay
logic; throughput and hardware authentication require the physical experiment.

## Prepare the hardware run

The supplied runner is specific to the original lab. Before using it, adapt
`benchmarks/run_decrypt_isolation.sh` and the selected YAML together:

| Setting in the supplied runner/configurations | Required preparation |
| --- | --- |
| `ubuntu@soc` | SSH access to the target DPU and permission to run Docker |
| `/home/ubuntu/monitoring_decap_only` | Complete experiment source and native `build/monitoring_app` on the DPU |
| `../packet_gen_recvs` (resolved by the runner) | Build `dpdk_esp_exclusive_gen` and `dpdk_simple_recv` as described above |
| `remote` network namespace | Provision sender namespace and physical connectivity; creation commands are not included |
| TX `82:00.0/1`, DPU `03:00.0/1`, RX `01:00.0/1` | Replace with the two-port TX → DPU → host topology's device addresses |
| `pf0hpf`, `pf1hpf`, SF4/SF6 | Provision host representors and monitoring SF interfaces |
| Static MACs, `172.16.1.*`/`172.16.2.*` addresses | Match the actual ports and tunnel/inner addressing on both sides |
| TX lcores `0–16`, RX `45–47`, DPU `1–2` or `1–5` | Reserve available cores and record NUMA placement |
| DOCA `3.1.0-devel` and `3.1.0-devel-host` images | Obtain the images and record their immutable digests |
| `/dev/hugepages`, `/dev/infiniband` | Configure hugepages and RDMA/device access on host and DPU |

The historical host setup reserved 4 GiB of hugepages. Confirm capacity and
MTU/link settings on your system. The supplied script launches privileged
containers, accesses NICs, and mounts sysfs in the sender namespace; use the
experiment devices exclusively.

Keep SA keys, salts, IVs, SPI ranges, and flow counts consistent between the
sender and DPU configurations. These fixed keys/IVs are experiment fixtures.
With 16 TX workers and two ports, the exclusive generator uses eight SAs per
port. In the four-worker setup each worker should see four of the 16 SAs.

## Execute and validate

After preparing both builds and adapting the runner, execute from
`ipsec-arm-encap/` on the host, outside a container:

```sh
# Unique lowercase-alphanumeric labels prevent collisions with retained logs.
bash benchmarks/run_decrypt_isolation.sh sweep1 7200 repro1a 16 static
bash benchmarks/run_decrypt_isolation.sh sweep1 7800 repro1b 16 static
bash benchmarks/run_decrypt_isolation.sh sweep 1400 repro4a 16 static

# A matched pacing comparison; these are new runs, not the retained SF points.
bash benchmarks/run_decrypt_isolation.sh sweep_sf0 1600 reprosf0a 16 static
bash benchmarks/run_decrypt_isolation.sh sweep_sf1 1600 reprosf1a 16 static
```

Run sequentially. TX lasts 30 seconds, RX 45 seconds, with additional startup
and shutdown time (roughly a minute per successful point). Repeat with new
labels and retain every triplet. Smaller pacing values send faster; pacing is
CPU-dependent and is not a calibrated rate. The `baseline`/`sweep2` variants
reference YAML files absent from this checkout; use the documented variants.

The runner starts the SoC, waits for pipeline readiness, starts RX, waits for
RX workers, then starts TX. It collects TX/RX/SoC logs under `benchmarks/` and
removes the run containers after normal completion. Timeout status 124 can be
normal for the timed sender/receiver. On early failure, containers may remain:
inspect the exact run's containers locally and on the DPU before retrying.

For each trial check:

1. All three logs exist and contain complete traffic statistics and shutdown
   counters; an absent summary row is not a successful zero-throughput result.
2. `RSS SA=` and `ESP totals queue=` show expected SA-to-worker placement.
3. Record SoC RX misses, software TX/replay drops, and host missed/no-mbuf
   counters separately. High TX throughput alone does not establish delivery.
4. For SF fraction 0.0, monitor-output traffic is zero; for 1.0 it is nonzero.
   Snapshot `ip -s link show dev en3f0pf0sf4` and `en3f1pf1sf6` on the DPU before
   and after each run to check SF TX counter deltas. Packet-content validation
   needs an additional capture/check procedure.
5. Re-run the CSV extraction above and save commands/configurations and the
   environment manifest with raw logs. Match offered load and other settings
   when attributing a difference to sampling.
