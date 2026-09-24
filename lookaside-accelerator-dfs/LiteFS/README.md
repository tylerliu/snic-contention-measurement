# LiteFS: A DPDK- and DOCA-Powered Distributed File System Prototype

LiteFS is a prototype of a high-performance, NIC-accelerated distributed file system. It leverages the NVIDIA DOCA and DPDK frameworks to offload filesystem operations to a BlueField DPU, enabling efficient, low-latency data transfers.

LiteFS is a stripped-down version of [LineFS](https://github.com/casys-kaist/LineFS) for measuring the host-to-SmartNIC log-processing and replication path. It replaces LineFS’s custom RDMA/RPC stack with DOCA and DPDK components and drives a synthetic write workload rather than a full filesystem deployment.

For the `aes-gcm` log-encryption setting, the accelerator produces GCM ciphertext and an authentication tag. The log processor retains the ciphertext and omits the authentication tag. Report this variant as **GCM (with auth tag omitted)**.

## Core Components

1.  **`hostfs`**: The host-side application. It allocates a DMA buffer, exports it for the NIC to access using DOCA DMA, and simulates a workload.
2.  **`nicfs`**: The core file system engine running on the SmartNIC/DPU. It receives access to the host's buffer, performs DMA transfers, and contains logic for processing data (e.g., compression, encryption) and replicating it to a peer NIC.
3.  **`nic_tester`**: An RDMA replica client that connects to `nicfs`, receives replicated log data, acknowledges doorbells, and reports interval byte rates.

## Dependencies

- **Meson** and **Ninja** build tools
- **DPDK** (`>= 22.11`)
- **NVIDIA DOCA** (3.1 for reproduction), including:
  - `doca-common`
  - `doca-dma`
  - `doca-dpdk-bridge`
- **libibverbs** and **libyaml** development libraries

Install the dependencies natively on the host and BlueField ARM systems. Ensure that `pkg-config` is configured to find the DPDK and DOCA libraries. Follow the [native setup and builds](../README.md#native-setup-and-builds) for SDK paths and machine-specific commands, or open the [optional DOCA development shell](../../README.md#doca-31-docker-setup) and run the same commands directly inside it.

## Building the Project

The project uses the Meson build system.

1.  **Navigate to the `LiteFS` directory:**
    ```bash
    cd /path/to/LiteFS
    ```

2.  **Set up the build directory:**
    ```bash
    meson setup build --buildtype=release -Denable_crypto=true
    ```
    This enables the crypto variants used in Scenario 1.

3.  **Compile the project:**
    ```bash
    ninja -C build
    ```

The compiled executables are `build/hostfs/hostfs`, `build/nicfs/nicfs`, and `build/nic_tester/nic_tester`.

## Running LiteFS

Build separately on ARM and host with `--buildtype=release -Denable_crypto=true`. Executables are in `build/nicfs/nicfs`, `build/hostfs/hostfs`, and `build/nic_tester/nic_tester`, rather than directly under `build`.

Copy the supplied YAML files into a run directory and update PCI addresses, IP addresses, verbs device names, and cores. Verify verbs devices in each process's network namespace; names are local to that namespace. In the lab, SoC `mlx5_2` is the SF carrying `172.16.1.2`, host `mlx5_2` is the BlueField host function, and peer `mlx5_0` in `remote` is ConnectX-7. These names must be checked on a new installation.

Start the server on the SoC first:

```sh
sudo ./build/nicfs/nicfs --config nicfs_params.yml
```

It waits for `hostfs` and all identities in `replicas`. The supplied `nic_tester` replica is an identity, not a hostname or `host:port` address. Then start these clients in separate terminals:

```sh
# Host default namespace
sudo ./build/hostfs/hostfs --config hostfs_params.yml
# Peer NIC namespace
sudo ip netns exec remote ./build/nic_tester/nic_tester --config nic_tester_params.yml
```

Both clients connect to `172.16.1.2:8080` in the supplied files. `nic_tester` accepts `primary-ip`, `primary-tcp-port`, and `ibv-device`; the old `role`, `port`, and `dest-mac` options are not supported. Wait for successful RDMA handshakes and tester `STATS` before measuring.

Set `program_flags.log-encryption` to `none`, `aes-xts`, or `aes-gcm`. XTS uses `03:00.0,class=crypto,algo=0`; GCM needs `algo=1` in the same YAML. Report GCM as **GCM (with auth tag omitted)**. Keep replica, core placement, batch sizes, and measurement interval fixed.

`hostfs` continuously generates batches from 256 KiB to just under 8 MiB, with a first create phase followed by rewrites; it does not load `workload.conf`. Use `timeout -s INT -k 5 <seconds>` to bound a run and stop every component after the measurement. Tester `STATS` reports interval replicated log bytes and B/s; `MB/s` is MiB/s. Exclude startup/shutdown and average stable intervals. This is replicated log throughput, because the program does not provide a finite completed-write byte count.

Use the [Scenario 1 temperature and cooldown procedure](../README.md#temperature-logging-and-cooldown) for each point.

Set data-link MTUs large enough to support RDMA MTU 4096 for all three LiteFS variants. Configure SoC physical `p0`, host/SF representors `pf0hpf`/`en3f0pf0sf0`, SoC SF, host interface, and peer interface inside `remote` consistently; include the corresponding port-1 interfaces when traffic uses both links. Endpoint updates can propagate MTUs to representors, so inspect the entire path afterward. All three component logs must print the same `Selected port MTU`; kernel MTU alone is insufficient. Keep these large link MTUs between experiments. Use DPDK **`-z 1472` for all LiteFS concurrent traffic runs**. Leave management interfaces unchanged. This is LiteFS prototype coverage; full LineFS was not deployed.

## Concurrent traffic comparisons

The experiment has exactly **nine cases**: three variants (`none`, `aes-xts`, and **GCM with auth tag omitted** / `aes-gcm`) × three conditions (no cross-traffic, host-to-network, and network-to-host). Keep data-link MTUs large enough to support RDMA MTU 4096 and use 1472-byte UDP payloads (`-z 1472`). Finish LiteFS bootstrap and verify nonzero replica throughput before starting the receiver and sender. Measure tester byte rates and actual network TX/RX rates over a common stable interval, retaining drop counters and temperature logs. Follow the [nine-case native experiment procedure](../README.md#litefs-traffic-experiment-grid) for endpoint placement, namespace entry, cooldowns, and analysis.
