# Independent ARM-to-host baseline — 2026-09-22

The ARM-to-host path works at high rate with DOCA 3.1 egress forwarding.
No decrypt, decap, replay checks, or ARM RX loop ran in this benchmark.
The production decryption sources were not modified.

## Results

Host-received aggregate across both PFs, 1456-byte Ethernet frames excluding FCS
(1414-byte UDP payload, matching the previous decrypt run's output frame size):

| ARM TX workers | TX queues per PF | Host RX workers | Received Mpps | Received Gib/s | Host RX missed / no_mbuf |
|---:|---:|---:|---:|---:|---:|
| 2, no DOCA rules | 1 | 2 | No benchmark traffic | 0 | 0 / 0 |
| 2 | 1 | 2 | 10.097449 | 109.537580 | 0 / 0 |
| 4 | 2 | 2 | 17.837608 | 193.503175 | 0 / 0 |
| 8 | 4 | 2 | 33.848792 | 367.193235 | 0 / 0 |

Each DOCA trial used a 30-second sender and 45-second receiver. Rates are the
mean of the last 15 receiver samples above 95% of that trial's peak, excluding
startup/shutdown, consistent with earlier benchmark reports. Single trials,
not a statistical capacity estimate. Gib/s = pps * 1456 * 8 / 2^30.
The tools label their 2^20-byte rates `MB/s`, but these are actually MiB/s.
Zero reported RX misses is not a sequence-number-based losslessness test.

Final DOCA egress rule packet counts:

| ARM workers | PF0 → pf0hpf | PF1 → pf1hpf |
|---:|---:|---:|
| 2 | 143,008,247 | 139,407,109 |
| 4 | 252,335,248 | 246,760,901 |
| 8 | 478,262,174 | 468,872,115 |

## Path and implementation

`arm_simple_gen` worker → its exclusive ARM PF TX queue → DOCA EGRESS root
(destination MAC match) → corresponding host representor → host `dpdk_simple_recv`.

- ARM PFs: `03:00.0` and `03:00.1`; host representors `pf0hpf` and `pf1hpf`.
- Host receiver: `01:00.0` and `01:00.1`, main lcore 45 and RX workers 46–47.
- ARM main lcore 1; workers 2–3, 2–5, or 2–9; workers alternate ports and own
  distinct queues. The inherited log's `core 2` label is misleading: the
  `Worker thread N` field is the actual lcore ID.
- One independent PF switch context and one MAC-matching root per PF, as in
  the decrypt application's software egress path. Misses drop. No SFs or OVS
  forwarding changes, no ESP or secure-ingress pipes.
- Generator batch 64, 1024 TX descriptors, default 65,536-mbuf pool.
- Fixed destination MACs bypass ARP: `58:a2:e1:53:19:d6` / `...:d7`.
- `arm_simple_gen.c` is a benchmark-only copy of `dpdk_simple_gen.c`, with
  corrected pending-mbuf ownership after partial bursts and optional DOCA hooks.
  Shared testing_tools sources were left unchanged.
- `arm_host_flow.cpp` implements only device setup, egress rules and counters;
  it reuses the repository's DeviceManager and utility helpers.
- One early DOCA attempt had a misplaced shutdown hook and crashed. It was
  fixed before all three reported trials; its `armhost-doca-failed-*` logs are
  retained but excluded from measurements.

## Interpretation

The same PF → egress → host-representor route can deliver about 30 times the
12.126 Gib/s observed with the decrypt pipeline. It is not intrinsically capped
at that low rate. This does not identify the decrypt pipeline's TX-drop cause:
the standalone generator uses fresh mbufs, no authentication metadata, no
simultaneous ARM ingress, checksum offloads, a different burst size, and retains
unsent packets for retry. The decrypt path reuses RX mbufs and immediately frees
unsent packets. Those are the next controlled comparisons, not proof that
retries alone fix throughput.

## Reproduce

Do not run concurrently with the decrypt application or another owner of these
DPDK ports. Both machines retain the user's existing 4 GiB hugepage reservation.

Deploy from the repository root:

```sh
ssh ubuntu@soc 'mkdir -p /home/ubuntu/arm-host-bench'
scp benchmarks/arm_simple_gen.c benchmarks/arm_host_flow.cpp \
    benchmarks/build_arm_host.sh device_manager.cpp device_manager.h utils.c utils.h \
    /home/tylerliu/partial_decrypt/testing_tools/dpdk_common.c \
    /home/tylerliu/partial_decrypt/testing_tools/dpdk_common.h \
    ubuntu@soc:/home/ubuntu/arm-host-bench/
ssh ubuntu@soc 'sudo docker run --rm \
    -v /home/ubuntu/arm-host-bench:/workspace -w /workspace \
    nvcr.io/nvidia/doca/doca:3.1.0-devel bash build_arm_host.sh'
bash benchmarks/run_arm_host.sh 2
bash benchmarks/run_arm_host.sh 4
bash benchmarks/run_arm_host.sh 8
```

The runner contains complete container commands, saves TX/RX logs under
`benchmarks/armhost-doca-N-{tx,rx}.log`, and removes stopped containers.
Expected normal timeout exit is 124. Inspect logs for initialization errors.
The test-specific idle hugepage mapping files were removed after these trials;
neither machine's reservation was changed. All test containers are stopped and
removed. Failed-run logs and naive-path logs are retained alongside the results.
