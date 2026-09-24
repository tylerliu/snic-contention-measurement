# Reproduce the monitoring experiments

This directory contains three separate DOCA Flow applications for Scenario 3. Build and run one variant at a time on the BlueField ARM system. Each subdirectory produces an executable named `monitoring_app`; its `monitoring_app_params.yml` is the starting configuration for that variant.

| Variant | Pipeline setting in the supplied YAML | Experiment |
| --- | --- | --- |
| [`cloning_only`](cloning_only/) | UDP port 3333, main destination `rss`, clone destination `drop`, mirroring enabled | First subsection: cloning without IPsec. Change `clone-dest` to `rss`, `host`, or a device when testing delivery of copies. |
| [`ipsec_cloning`](ipsec_cloning/) | ESP/IPsec decapsulation and encapsulation, anti-replay enabled by default, monitor destination `dpdk` | Second subsection: IPsec cloning with anti-replay off and on. |
| [`ipsec_sampling`](ipsec_sampling/) | Same IPsec configuration, monitor destination SF4/SF6, `sampling-fraction: 0.99` | Final application: IPsec sampling with Suricata 8.0.2 receiving the monitor copies. |

These are different programs, not command-line modes of one executable. In particular, `cloning_only` does not configure IPsec, and its sample `clone-dest: drop` is useful for measuring mirroring overhead but does not deliver monitor copies.

## Testbed and prerequisites

Use the dual-port BlueField-3/ConnectX-7 topology and 1500-byte link MTU in the [repository setup](../README.md#environment-setup). The lab mapping in the supplied YAML is BlueField PFs `03:00.0`/`03:00.1`, host representors `pf0hpf`/`pf1hpf`, and, for `ipsec_sampling`, monitor SFs `en3f0pf0sf4`/`en3f1pf1sf6`. Replace those names, PCI addresses, MAC addresses, and IP addresses with the corresponding values on your testbed. Provision the SFs before running the sampling variant.

Build on the BlueField ARM system with Linux, Meson/Ninja, a C/C++ compiler, and pkg-config entries for `libdpdk`, `doca-common`, `doca-flow`, `doca-dpdk-bridge`, and `yaml-0.1`. Configure DPDK hugepages and device access on ARM and host before starting traffic. Run each variant with exclusive use of its PFs and CPU cores.

## Optional DOCA 3.1 Docker shells

Use the shared [DOCA 3.1 Docker setup](../README.md#doca-31-docker-setup) for the monitoring applications and host traffic tools. Copy the sources as described below before opening a container. On the host, start from the repository root; on the SoC, start from the copied variant directory, such as `~/monitoring_ipsec_sampling`. Install missing build dependencies inside the image, including `libyaml-dev` and `libssl-dev`.

The Suricata setup and capture commands below use the native SoC installation. Suricata is installed separately from the DOCA image; keep its devices and cores separate from the containerized monitoring application.

## Build the applications natively

Install the DOCA 3.1 SDK and its DPDK development libraries on the BlueField SoC, and the matching DPDK development libraries on the two-port traffic host. Install Meson, Ninja, a C/C++ compiler, pkg-config, libyaml development headers, and OpenSSL development headers. Configure `PKG_CONFIG_PATH` for the installed SDK if its `.pc` files are outside the system search path. Before building, check `pkg-config --modversion libdpdk` on both machines and `pkg-config --exists doca-common doca-flow doca-dpdk-bridge yaml-0.1` on the SoC. Configure hugepages and access to the NIC and RDMA devices on both machines. Build and run each binary against the same native DOCA/DPDK installation. On systems with NVIDIA packages under `/opt/mellanox`, set the pkg-config path on both host and SoC before building (adjust it if your SDK is elsewhere):

```sh
sdk_arch="$(uname -m)-linux-gnu"
export PKG_CONFIG_PATH="/opt/mellanox/dpdk/lib/$sdk_arch/pkgconfig:/opt/mellanox/doca/lib/$sdk_arch/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --modversion libdpdk
```

The variants share `cloning_only/arg_parser`; the other two parser directories
are relative symlinks. From the repository root on the host, copy the three
variants to the SoC, dereferencing symlinks so each deployed copy is self-contained:

```sh
for variant in cloning_only ipsec_cloning ipsec_sampling; do
  rsync -aL --exclude build --exclude 'build-*' "monitoring/$variant/" \
    "soc:~/monitoring_$variant/"
done
```

In a SoC shell, build each variant separately. The example below builds the sampling application; substitute the other directory names for the first two benchmarks. Use `meson compile -C build` alone for later source changes.

```sh
cd "$HOME/monitoring_ipsec_sampling"
meson setup build --buildtype=release
meson compile -C build
```

Run an application from its own directory so its YAML path resolves. For example, in a SoC shell:

```sh
cd "$HOME/monitoring_ipsec_sampling"
sudo stdbuf -oL ./build/monitoring_app --config monitoring_app_params.yml
```

The sample YAML selects EAL lcores `0-2`: main lcore 0 and queue lcores 1-2. With SF destinations, `ipsec_sampling` does not start DPDK polling workers on those queue lcores. Start the application before traffic and wait for `pipeline build complete`. Stop it with Ctrl-C after TX finishes so its final counters are retained. Run one variant at a time; do not let two applications own the same PFs.

On the traffic host, build the shared-SA ESP generator and four-worker receiver natively:

```sh
cd packet_gen_recvs
meson setup build-doca31 --buildtype=release
meson compile -C build-doca31 dpdk_esp_gen dpdk_simple_recv dpdk_simple_gen
```

For the IPsec variants, match the sender's ESP SPI, AES-GCM key, salt, IV, tunnel addresses, and MAC addresses to the `ipsec-decap-*` YAML entries. The supplied app installs one inbound SA per PF. Use `dpdk_esp_gen` for multi-core runs because it shares the ordered sequence for each PF's SA; `dpdk_esp_exclusive_gen` assigns additional SPIs that need additional decrypt rules. With explicit `--dst-mac`, ARP is bypassed; verify the MAC against the PF hardware address. The `ipsec-encap-*` entries describe the reverse direction. The fixed values in these files are lab fixtures, not credentials to reuse on another network.

## Traffic and comparisons

Keep generator, packet size, link MTU, core assignment, port mapping, and offered TX rate fixed within a comparison. Follow the paper's subsection order:

1. **Cloning only:** compare `disable-mirroring: true` and `false` in `cloning_only/monitoring_app_params.yml`. Send UDP traffic to destination port 3333 with `dpdk_simple_gen`. Record `main-dest` and `clone-dest`; the supplied `clone-dest: drop` measures the mirror path without a receiving monitor.
2. **IPsec cloning:** copy `ipsec_cloning/monitoring_app_params.yml` for two runs. Set `ipsec-decap-anti-replay: false` in one and `true` in the other. Keep `monitor-dest: dpdk`, both SAs, and the traffic identical. Restart the application between settings and report the two results separately.
3. **IPsec sampling with Suricata:** retain the SF monitor destinations in `ipsec_sampling/monitoring_app_params.yml`. Set `ipsec-decap-anti-replay: false` in a run-specific copy. Sweep **off** (`monitor-dest: none`), fraction **0** with the SF destinations retained, then 1/16, 1/8, 3/16, 2/8 through 7/8, and 1. Keep all other settings fixed. Use the native DPDK Suricata setup below.

### Build Suricata 8.0.2 on the SoC

The supplied [`suricata.yaml`](suricata.yaml) captures DPDK Ethernet devices `mlx5_core.sf.4` and `mlx5_core.sf.6`, reached through representors `en3f0pf0sf4` and `en3f1pf1sf6`. Provision those SFs first. It assigns seven workers per capture device (14 total), uses cores 1-14 for workers and core 15 for DPDK main/management, and leaves core 0 for the monitoring app. Suricata and `monitoring_app` are separate DPDK processes: keep their devices, cores, hugepages, and EAL file prefixes separate.

Copy the build script and YAML from the repository root, then on the SoC run the script directly on the installed DOCA 3.1 system. The script uses `apt-get` on Debian/Ubuntu, installs its open-source build dependencies, builds upstream VectorScan 5.4.12 in release mode, and builds Suricata 8.0.2 with DPDK support under the same `suricata-build` directory. If the DOCA pkg-config files need a custom path, preserve `PKG_CONFIG_PATH` through `sudo`.

```sh
# On the repository host:
ssh soc 'mkdir -p "$HOME/suricata-build"'
scp monitoring/setup_suricata.sh monitoring/suricata.yaml soc:~/suricata-build/

# In a SoC shell:
cd "$HOME/suricata-build"
sudo --preserve-env=PKG_CONFIG_PATH bash ./setup_suricata.sh vectorscan
# For a build without VectorScan, use "basic" instead of "vectorscan".
```

Prepare the rules and paths named in `suricata.yaml`. Use the same Emerging Threats Open ruleset snapshot for every comparison. The update command below fetches the current ruleset; save that file and reuse it across runs. The script keeps the binary at `install-hs/bin/suricata`, with `libhs.so.5` under `/usr/local/lib`. Use that library path for validation and runtime so the loader does not select an older distro library.

```sh
cd "$HOME/suricata-build"
sudo install -d /etc/suricata /var/lib/suricata/rules /var/log/suricata
sudo install -m 644 suricata.yaml /etc/suricata/suricata.yaml
sudo install -m 644 suricata-8.0.2-vectorscan/etc/classification.config \
  /etc/suricata/classification.config
sudo install -m 644 suricata-8.0.2-vectorscan/etc/reference.config \
  /etc/suricata/reference.config
sudo install -m 644 suricata-8.0.2-vectorscan/threshold.config \
  /etc/suricata/threshold.config
sudo env LD_LIBRARY_PATH=/usr/local/lib ./install-hs/bin/suricata-update \
  --suricata "$PWD/install-hs/bin/suricata" \
  --suricata-conf /etc/suricata/suricata.yaml \
  -o /var/lib/suricata/rules --no-test
env LD_LIBRARY_PATH=/usr/local/lib ./install-hs/bin/suricata --build-info \
  | grep -E 'DPDK support|Hyperscan support'
env LD_LIBRARY_PATH=/usr/local/lib ldd ./install-hs/bin/suricata \
  | grep libhs.so.5
sudo env LD_LIBRARY_PATH=/usr/local/lib ./install-hs/bin/suricata \
  -c /etc/suricata/suricata.yaml -T
```

Expect DPDK and Hyperscan support to be `yes`, `libhs.so.5` to resolve from `/usr/local/lib`, and the full ruleset validation to pass. The supplied YAML uses `mpm-algo: auto` and `spm-algo: auto`, which select VectorScan in this build. Its stats interval is six seconds.

### Run one sampling point without containers

Use a fresh run label and log directory for every point. In one SoC shell, start Suricata and wait for `Threads created -> W: 14 ... Engine started` in `suricata.log` before sending traffic. Run it with native CPU affinity restricted to cores 1-15:

```sh
run_dir="$HOME/monitoring-runs/sampling_001"
mkdir -p "$run_dir/suricata"
sensors | sed -n '/mlx5-pci-0300/,+2p;/mlx5-pci-0301/,+2p' \
  > "$run_dir/pre-run-nic-temperature.log"
cd "$HOME/suricata-build"
sudo taskset -c 1-15 env LD_LIBRARY_PATH=/usr/local/lib \
  ./install-hs/bin/suricata -c /etc/suricata/suricata.yaml \
  -l "$run_dir/suricata" --dpdk 2>&1 | tee "$run_dir/suricata.stdout.log"
```

In a second SoC shell, copy the app configuration, set anti-replay off, and run it. For **off**, replace the `monitor-dest` list in the run-specific YAML with a single `- "none"`; for **0**, retain both SF destinations and set `sampling-fraction: 0`. Change only the fraction for subsequent points. Wait for `pipeline build complete` before starting the receiver and sender.

```sh
run_dir="$HOME/monitoring-runs/sampling_001"
cd "$HOME/monitoring_ipsec_sampling"
cp monitoring_app_params.yml sampling_001.yml
sed -i 's/ipsec-decap-anti-replay: true/ipsec-decap-anti-replay: false/' \
  sampling_001.yml
sed -i 's/sampling-fraction: 0.99/sampling-fraction: 0.625/' sampling_001.yml
sudo timeout -s INT -k 10 140 stdbuf -oL ./build/monitoring_app \
  --config sampling_001.yml 2>&1 | tee "$run_dir/soc.log"
```

On the traffic host, run the clear receiver in one shell. This example uses four RX workers on lcores 44-47; lcore 43 is its main thread. Keep the receiver up for the full 48-second send:

```sh
cd packet_gen_recvs
mkdir -p monitoring-run-001
sudo timeout -s INT -k 5 70 stdbuf -oL ./build-doca31/dpdk_simple_recv \
  -a 0000:01:00.0 -a 0000:01:00.1 -l 43-47 -W 4 -M 16384 \
  -f sampling-001-rx --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 \
  -p 0 -S 2>&1 | tee monitoring-run-001/rx.log
```

In a second host shell, start the SoC NIC temperature loop, then the **shared-SA** ESP sender. The two destination MACs are the lab's BlueField PF hardware addresses; update them with the YAML and physical setup. `--vary-src-ip` changes visible outer source IPs for SF RSS distribution while retaining each PF's SPI. The sender uses 42 TX workers on lcores 1-42; lcore 0 is its main thread. The `remote` network namespace must exist and contain the two TX ports.

```sh
cd packet_gen_recvs
ssh soc 'for i in $(seq 1 56); do date -u +%FT%TZ; \
  sensors | sed -n "/mlx5-pci-0300/,+2p;/mlx5-pci-0301/,+2p"; sleep 1; done' \
  > monitoring-run-001/soc-nic-temperature.log &
sensor_pid=$!
sudo ip netns exec remote timeout -s INT -k 5 48 stdbuf -oL \
  ./build-doca31/dpdk_esp_gen \
  -a 0000:82:00.0 -s 172.16.1.20 -d 172.16.1.128 \
  --dst-mac 58:a2:e1:53:19:d6 -T 172.16.1.2 --spi 0x2001 \
  --key aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899 \
  --salt 0x22334455 --iv 0x1234567890abcdef \
  -a 0000:82:00.1 -s 172.16.2.20 -d 172.16.2.128 \
  --dst-mac 58:a2:e1:53:19:d7 -T 172.16.2.2 --spi 0x2002 \
  --key 99887766554433221100ffeeddccbbaa99887766554433221100ffeeddccbbaa \
  --salt 0x66778899 --iv 0xfedcba0987654321 \
  -l 0-42 -f sampling-001-tx -z 1414 -p 3333 -t 0 -S \
  --vary-src-ip 2>&1 | tee monitoring-run-001/tx.log
wait "$sensor_pid"
```

After the sender finishes, stop the SoC app and Suricata with Ctrl-C in their shells. The receiver exits at 70 seconds. Save the run-specific YAML and raw counters on the host:

```sh
scp soc:~/monitoring_ipsec_sampling/sampling_001.yml monitoring-run-001/
scp soc:~/monitoring-runs/sampling_001/soc.log monitoring-run-001/
scp soc:~/monitoring-runs/sampling_001/pre-run-nic-temperature.log \
  monitoring-run-001/
scp soc:~/monitoring-runs/sampling_001/suricata/stats.log monitoring-run-001/
scp soc:~/monitoring-runs/sampling_001/suricata/suricata.log monitoring-run-001/
```

Suricata writes logs as root. If your SSH user cannot read them, copy them to the run directory with `sudo install -m 644` on the SoC before `scp`. Wait about 60 seconds with traffic and SoC apps stopped before the next point, then record a fresh pre-run NIC temperature. The 48-second send provides at least seven complete six-second Suricata intervals after startup. `timeout` may return status 124 when the set duration expires.

## Read the statistics

For the first two subsections, wait until roughly five `=== monitoring snapshot ===` blocks have appeared after traffic starts, then read the next steady snapshot's throughput. Retain all blocks and the sender/receiver logs so the selected point can be checked. The monitoring applications print a `=== monitoring snapshot ===` about once per second. Read samples during the steady traffic interval, excluding startup and shutdown. Their `pkt/s` and `MB/s` values are interval rates; in the code `MB/s` divides bytes by 1024², so treat the number as MiB/s. The `Port ... RX rate` line also includes cumulative `missed`, `errors`, and `no_mbuf` counts. A higher sender TX rate by itself is not proof of delivered traffic.

| Source | What to read | Interpretation |
| --- | --- | --- |
| Sender and clear-host receiver | TX and RX rates, RX missed/no-mbuf | Compare delivered clear traffic at the host under the same offered load. |
| `cloning_only` ARM log | `DOCA RSS (Main) pipe`, other enabled destination pipes, worker/port RX | Check main-path activity and compare host/ARM rates with mirroring disabled and enabled. A `clone-dest: drop` run has no receiving clone endpoint. |
| IPsec ARM logs | `DOCA Host decrypt pipe`, `DOCA Host decap pipe`, `DOCA To-Host pipe`, `DOCA Monitor output pipe`, `DOCA Drop pipe` | Check that ESP reaches decryption, clear traffic reaches the host, and monitor traffic follows the selected branch. A drop-pipe count is not a host RX count. |
| `ipsec_cloning` ARM log | Worker and `Port ... RX rate` plus DOCA pipe rates | Compare anti-replay off/on using the same traffic. Keep `missed`, `errors`, and `no_mbuf` alongside the rate. |
| `ipsec_sampling` ARM log | `DOCA Monitor output pipe` and `DOCA To-Host pipe` | Monitor output should be zero at fraction 0.0 and rise when sampling is enabled. Compare the host branch independently. |
| Suricata `stats.log` and `eve.json` | Per-interval capture/decoder counters, drops, and alerts | Use packet-counter deltas to confirm capture and decode. Check protocol counters before claiming payload inspection; alerts depend on rules and packet contents. |

For the sampling sweep, use separate points for **off** (`monitor-dest: none`) and fraction **0** (monitor destination retained, `sampling-fraction: 0`), followed by 1/16, 1/8, 3/16, and 2/8 through 7/8, then 1. Take **Host Receiving** from the fifth stable one-second `TOTAL RX` line, **Monitor Sampled** from the sum of both `DOCA Monitor output pipe` lines in a stable ARM snapshot, and **ARM Processed** from the change in Suricata `decoder.bytes` over the second complete six-second interval after traffic starts. Convert the Suricata delta with `delta_bytes * 8 / (elapsed_seconds * 2^30)` Gib/s. The tools label MiB/s as `MB/s`, so divide their one-second values by 128 for Gib/s. Keep the exact raw counter values and timestamps for each rate. Check the matching `capture.dpdk.imissed` delta and shutdown per-device drop summary; a high decoded rate with many misses is lossy. Suricata 8.0.2 signals worker counter synchronization every 3 seconds. The 6-second output interval usually spans two counter updates; check adjacent intervals for delayed updates and also calculate the rate from the first seven complete intervals (42 seconds) to include catch-up. Wait about 60 seconds after each run before starting the next so the SoC NIC temperature can fall, and record its reading before the next run. Compare `decoder.esp` with `decoder.udp`: this pipeline mirrors ESP before the host decrypt pipe, so it captures encrypted packets. The generator varies visible outer source IPs with `--vary-src-ip` to distribute ESP among the SF RSS queues while keeping each PF's SPI matched to its inbound SA. The 14-worker Suricata layout leaves SoC core 0 for the monitoring app.

## Record temperatures during traffic

Run `sensors` on the SoC immediately before each trial, then sample only the two BlueField NIC ASIC sensors throughout traffic and once after shutdown. Timestamp each reading so it can be matched to throughput:

```sh
while true; do date -u +%FT%TZ; sensors | sed -n "/mlx5-pci-0300/,+2p;/mlx5-pci-0301/,+2p"; sleep 1; done | tee soc-nic-temperature.log
```

Stop the loop after the run. Compare each NIC ASIC temperature with its reported `crit` limit and check whether throughput falls as temperature rises. Temperature alone does not prove throttling; retain any device throttle counters or firmware events if available. Save the exact YAML, command lines, software versions, raw counters, and timestamped SoC NIC temperatures with each run.
