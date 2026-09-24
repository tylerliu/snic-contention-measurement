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

The reported result uses `decrypt_16tx_4arm_sf1.yml`: four ARM workers, sampling fraction 1.0,
16 TX workers, eight SAs per port, and 1414-byte UDP payloads. Its clear-host RX
rate is approximately **183 Gib/s**.

The Scenario 3 result is **87.59046875 Gib/s**. In that scenario,
enabling IPsec ESP anti-replay increases throughput loss to 69.0%
without sampling and to 70.2% as sampling increases. The ARM operating point
is **2.09×** the Scenario 3 rate. Both experiments use a **1500-byte link MTU**, a **dual-port DPDK
application without link aggregation**, the **same hardware setup**, and the
**same receiver**.

## Included materials

- `monitoring_*.cpp`, `device_manager.*`, `arg_parser/`: application and configuration.
- `decrypt_16tx_1arm.yml`, `decrypt_16tx_4arm.yml`: one/four-worker baselines.
- `decrypt_16tx_4arm_sf0.yml`, `decrypt_16tx_4arm_sf1.yml`: monitor destinations
  enabled with sampling fraction 0.0/1.0.
- `esp_layout_test.cpp`, `esp_replay_test.cpp`: layout and replay-window tests.

Hardware correctness checks are described in [DECAP_ONLY_TEST.md](DECAP_ONLY_TEST.md),
using `test_decap_host.py` and the `phase1_decap_only*.yml` configurations.

## Build and component checks

Use the shared [DOCA 3.1 Docker setup](../README.md#doca-31-docker-setup) for interactive build and runtime shells. Use those shells for the commands below, with device and hugepage access configured.

The application requires Linux, C/C++ compilers, Meson/Ninja, DPDK,
`doca-common`, `doca-flow`, `doca-dpdk-bridge`, libyaml (`yaml-0.1`), and pthreads.
Use the DOCA 3.1 ARM environment with its pkg-config paths configured. From the repository root of a complete checkout on the DPU:

```sh
meson setup ipsec-arm-decap/build ipsec-arm-decap
meson compile -C ipsec-arm-decap/build
meson test -C ipsec-arm-decap/build --print-errorlogs
```

### Build the host generator and receiver

Use these targets from [packet_gen_recvs](../packet_gen_recvs/):

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
The ESP generator uses `-d` for inner destination IP, `-T` for tunnel destination,
`--dst-mac` for static Ethernet destinations, and `-t` for pacing. With
`-l 0-16`, one main lcore and 16 TX workers are selected; the exclusive
generator automatically assigns eight SAs per port across the two ports.
`dpdk_esp_gen` is the separate shared-SA generator and does not match the multi-SA configuration used here.

### Component checks without hardware

On a machine without DOCA/DPDK, run the two header-only component tests with
a C++11 compiler. From the repository root:

```sh
check_dir=$(mktemp -d)
c++ -std=c++11 -Wall -Wextra ipsec-arm-decap/esp_layout_test.cpp -o "$check_dir/esp_layout_test"
c++ -std=c++11 -Wall -Wextra ipsec-arm-decap/esp_replay_test.cpp -o "$check_dir/esp_replay_test"
"$check_dir/esp_layout_test" && "$check_dir/esp_replay_test"
```

Both exit successfully without output when their assertions pass. Keep
assertions enabled (do not add `-DNDEBUG`). These test layout parsing and replay
logic; throughput and hardware authentication require the physical experiment.

## Prepare the hardware run

Use the [repository networking setup](../README.md#networking-setup), adapting
PCI, MAC, and IP addresses in the commands and selected YAML together. Deploy
the complete `ipsec-arm-decap/` directory to ARM and build it there. On the
host, build the traffic tools from the same checkout. Reserve hugepages on
both systems and expose `/dev/hugepages` and `/dev/infiniband` to containers.
Configure enough hugepage memory for the application and traffic tools.

| Role | Devices | Lcores |
| --- | --- | --- |
| Host ESP TX in `remote` | `82:00.0`, `82:00.1` | Main 0; workers 1–16 |
| Host clear RX | `01:00.0`, `01:00.1` | Main 45; workers 46–47 |
| ARM processing | `03:00.0`, `03:00.1` | Main 1; workers 2–5 (four-worker case) |
| ARM monitoring output | `en3f0pf0sf4`, `en3f1pf1sf6` | SF interfaces |

Keep host representors `pf0hpf`/`pf1hpf`, static MAC addresses, and per-port
keys, salts, IVs, and SPI ranges consistent with the selected YAML. The fixed
crypto values are experiment fixtures. Sixteen TX workers supply eight SAs
per port; each of the four ARM workers should receive four SAs.

## Execute and validate

Commands below assume native DOCA 3.1 environments or the matching
interactive containers described in the repository setup. Use a separate
log directory for each run.

Start the pipeline on ARM, from the repository root, and wait for
`pipeline build complete`:

```sh
mkdir -p reproduction-runs/arm-decap
sudo stdbuf -oL ./ipsec-arm-decap/build/monitoring_app \
  --config ipsec-arm-decap/decrypt_16tx_4arm_sf1.yml \
  2>&1 | tee reproduction-runs/arm-decap/arm.log
```

Start RX in a host terminal, from the repository root. Wait for both RX
workers to report ready before starting TX:

```sh
mkdir -p reproduction-runs/arm-decap
sudo timeout -s INT -k 5 45 stdbuf -oL ./packet_gen_recvs/build/dpdk_simple_recv \
  -a 0000:01:00.0 -a 0000:01:00.1 -l 45-47 -f arm-decap-rx \
  --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 -p 0 -S \
  2>&1 | tee reproduction-runs/arm-decap/rx.log
```

Start the sender promptly in a second host terminal so the full 30-second
TX interval fits within RX's 45 seconds. For container execution, enter the
`remote` namespace with the [shared namespace procedure](../README.md#doca-31-docker-setup)
and omit the `sudo ip netns exec remote` prefix below:

```sh
sudo ip netns exec remote timeout -s INT -k 5 30 stdbuf -oL \
  ./packet_gen_recvs/build/dpdk_esp_exclusive_gen \
  -a 0000:82:00.0 -s 172.16.1.20 -d 172.16.1.128 \
  --dst-mac 58:a2:e1:53:19:d6 -T 172.16.1.2 --spi 0x2001 \
  --key aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899 \
  --salt 0x22334455 --iv 0x1234567890abcdef \
  -a 0000:82:00.1 -s 172.16.2.20 -d 172.16.2.128 \
  --dst-mac 58:a2:e1:53:19:d7 -T 172.16.2.2 --spi 0x2002 \
  --key 99887766554433221100ffeeddccbbaa99887766554433221100ffeeddccbbaa \
  --salt 0x66778899 --iv 0xfedcba0987654321 \
  -l 0-16 -f arm-decap-tx -z 1414 -p 3333 -t 1550 -S \
  2>&1 | tee reproduction-runs/arm-decap/tx.log
```

After TX finishes, allow RX to exit and stop the ARM pipeline with Ctrl-C to
retain its final counters. Timeout status 124 is expected for timed processes.
Retain both machines' logs and exact YAML. Snapshot the two monitoring SFs
with `ip -s link show dev INTERFACE` on ARM before and after traffic.

Select the YAML for the configuration to measure:

| Configuration | ARM workers | Monitoring |
| --- | ---: | --- |
| `decrypt_16tx_1arm.yml` | 1 | Disabled |
| `decrypt_16tx_4arm.yml` | 4 | Disabled |
| `decrypt_16tx_4arm_sf0.yml` | 4 | SF branch, fraction 0.0 |
| `decrypt_16tx_4arm_sf1.yml` | 4 | SF branch, fraction 1.0 |

The sender's `-t` controls pacing; smaller values send faster. Keep pacing
fixed for comparisons and verify the actual offered load. Restart
the ARM app between trials to reset replay state. Monitoring disabled and SF0
are distinct: SF0 keeps the monitoring branch configured but drops its copies.

## Read the results

Measure `TOTAL TX` and `TOTAL RX` over the same steady-state interval.
For the 1414-byte UDP payload, convert packet rates as follows:

- Encrypted input Gib/s: mean TX packets/s × 1510 × 8 / 2³⁰.
- Clear host RX Gib/s: mean RX packets/s × 1456 × 8 / 2³⁰.

Frame lengths exclude FCS. Report delivered throughput at the clear-host
receiver, and distinguish steady-state rates from whole-run drop counters.

Check `RSS SA=` and `ESP totals queue=` for SA placement (all 16 SAs on one
worker in the one-worker baseline, four per worker in the four-worker cases).
Report SoC RX misses, software TX/replay drops, and host missed/no-mbuf counts
separately. SF0 should show zero monitor-output traffic; SF1 should show
nonzero traffic and corresponding SF TX counter deltas. Pipe counters alone
do not establish clone contents; use packet capture for content validation.
