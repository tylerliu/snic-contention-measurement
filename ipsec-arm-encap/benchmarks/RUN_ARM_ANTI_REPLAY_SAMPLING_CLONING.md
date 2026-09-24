# Run the ARM anti-replay experiment with sampling and cloning

The experiment's goal is to move ESP anti-replay enforcement from DOCA to
the ARM DPDK worker while preserving DOCA's packet sampling and cloning
branch. The original 1- and 4-worker throughput baselines use
`monitor-dest: none` and `sampling-fraction: 0.0`; they do **not** test
cloning. The four-worker SF comparison below enables monitor destinations
SF4/SF6 and tests sampling fractions 0.0 and 1.0.

Run these commands from `ipsec-arm-encap/` in this repository on the host,
**not inside a container**. The runner starts three
DOCA 3.1 containers: the pipeline on `ubuntu@soc`, then the host RX,
then the ESP sender in the `remote` network namespace. It stops TX first,
waits for RX, stops the SoC app, and saves the three container logs.
Do not run another DPDK experiment on these NICs concurrently.

## Prepare once after source changes

The SoC already has a prepared source tree at
`/home/ubuntu/monitoring_decap_only`. Copy the current datapath and
the two configurations, then compile with the **DOCA 3.1** image:

```sh
scp monitoring_doca.cpp monitoring_dpdk.cpp monitoring_dpdk.h \
  decrypt_16tx_1arm.yml decrypt_16tx_4arm.yml \
  decrypt_16tx_4arm_sf0.yml decrypt_16tx_4arm_sf1.yml \
  ubuntu@soc:/home/ubuntu/monitoring_decap_only/
ssh ubuntu@soc 'sudo docker run --rm --privileged --network host \
  -v /dev/infiniband:/dev/infiniband -v /dev/hugepages:/dev/hugepages \
  -v /home/ubuntu/monitoring_decap_only:/workspace -w /workspace \
  nvcr.io/nvidia/doca/doca:3.1.0-devel \
  sh -lc "meson compile -C build && meson test -C build --print-errorlogs"'
```

The local host has a different DOCA version; its full app build is not the
target build. The host also needs
`../packet_gen_recvs/build/dpdk_esp_exclusive_gen` and
`../packet_gen_recvs/build/dpdk_simple_recv`; follow the
[host build instructions](../README.md#build-the-host-generator-and-receiver).
The runner mounts this checkout's `packet_gen_recvs` directory into both host
containers. It also requires the `remote` network namespace visible under
`/var/run/netns`, and available DPDK hugepages. On this setup, reserve
4 GiB on the host when needed with
`sudo /opt/mellanox/dpdk/bin/dpdk-hugepages.py -r4G` before starting;
the runner does not pass `-M`. The experiment uses 82:00.0/1 for ESP TX,
03:00.0/1 for the SoC pipeline, and 01:00.0/1 for host RX. Static unicast
destination MACs are built into the runner, so ARP resolution is not needed.

## Run one point

The last three arguments are a **fresh lowercase-alphanumeric label**,
16 TX workers, and static unicast MAC mode. Labels must be unique because
the runner refuses to overwrite an existing RX log.

```sh
# One SoC worker: config decrypt_16tx_1arm.yml, main lcore 1, worker 2.
bash benchmarks/run_decrypt_isolation.sh sweep1 7200 one7200newa 16 static

# Four SoC workers: config decrypt_16tx_4arm.yml, main lcore 1, workers 2-5.
bash benchmarks/run_decrypt_isolation.sh sweep 1400 four1400newa 16 static
```

To find a peak rather than repeat one point, change only the sender
`-t` pacing value and label. Useful tested ranges are `6800–8500` for
one SoC worker and `1200–1600` for four. Smaller `-t` sends faster;
`-t` counts `rte_pause()` calls per sender batch, not a calibrated rate.
The current best tested delivered points are `7200` (one worker) and
`1400` (four workers). For a zero-reported-SoC-miss one-worker point,
test `7800`. Repeat candidate peaks with a new label.

Each run takes roughly a minute: 30 seconds of ESP TX, 45 seconds of host
RX, plus startup/shutdown. Normal `timeout` exit values of 124 for
the sender/receiver appear in runner output. If a run fails before
cleanup, inspect the exact tagged containers with `docker ps -a` and
`docker logs` locally and via `ssh ubuntu@soc` before retrying.

## Compare SF sampling and cloning (four workers)

Use `sweep_sf0` and `sweep_sf1` to hold sender pacing, SA count, host
receiver, and ARM worker count constant while changing only sampling
fraction. Both configs route monitor copies toward
`en3f0pf0sf4,dv_flow_en=2` and `en3f1pf1sf6,dv_flow_en=2`.
At fraction 0.0 the mirror branch is built but the sampling pipe drops
all copies; at 1.0 copies go directly to the monitor-output pipe and SFs.

```sh
bash benchmarks/run_decrypt_isolation.sh sweep_sf0 1600 sf0newa 16 static
bash benchmarks/run_decrypt_isolation.sh sweep_sf1 1600 sf1newa 16 static
bash benchmarks/summarize_decrypt_sweep.sh sweep_sf0 | rg '^(trial|decap-isolate-sweep_sf0-.*-sf0)'
bash benchmarks/summarize_decrypt_sweep.sh sweep_sf1 | rg '^(trial|decap-isolate-sweep_sf1-.*-sf1)'
```

Run the variants sequentially. For a stress comparison, repeat both at
`-t 1400`; those runs can incur SoC RX misses. Confirm that fraction 0.0
has zero `DOCA Monitor output pipe` traffic in the SoC log, whereas
fraction 1.0 has nonzero output. To verify actual SF delivery, snapshot
`ip -s link show dev en3f0pf0sf4` and
`ip -s link show dev en3f1pf1sf6` on `soc` before and after each run
and compare their TX packet counters. The measured comparison is in
`ARM_ANTI_REPLAY_SF_COMPARISON.md`.

## Read and retain results

The runner writes
`benchmarks/decap-isolate-sweep1-PAUSE-LABEL-static-{tx,rx,soc}.log`
for one worker, or `...-sweep-PAUSE-LABEL-static-...` for four.
Summarize the runs with your label prefix:

```sh
bash benchmarks/summarize_decrypt_sweep.sh sweep1 | rg '^(trial|decap-isolate-sweep1-.*-one)'
bash benchmarks/summarize_decrypt_sweep.sh sweep  | rg '^(trial|decap-isolate-sweep-.*-four)'
```

`rx_Gibps` is clear host Ethernet throughput; `tx_Gibps` is encrypted
ESP input. `rx_missed` is the SoC's whole-trial RX miss counter, while
`tx_drop` and `replay` come from SoC workers. Host missed/no-mbuf
counts are separate. The rate columns average samples 8–23; do not
compare them to whole-trial packet totals as if they covered the same
interval. Check `RSS SA=` and `ESP totals queue=` in the SoC log to
confirm all 16 active SAs appeared on the expected worker(s): one queue
for one worker, or four SAs on each of four queues. The configuration
provisions eight SAs per port; the generator sends one SA per TX worker.

Record the full sweep in a summary table, then retain raw TX/RX/SoC
triplets only for the peak and any separately reported zero-miss point.
The current reference results are
`SA_STEERING_SINGLE_WORKER_RETUNE.md`, `SA_STEERING_RETUNE.md`,
and `ARM_ANTI_REPLAY_SF_COMPARISON.md`.
