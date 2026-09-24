# ARM anti-replay with SF sampling/cloning: four-worker results

2026-09-23, DOCA 3.1, baseline datapath commit `ad555df`. Hardware
authenticates/decrypts ESP; four ARM DPDK workers perform decapsulation
and per-SA anti-replay. Sixteen TX workers generate eight SAs per port.
Both runs use SF4/SF6 as `monitor-dest`:
`en3f0pf0sf4,dv_flow_en=2` and `en3f1pf1sf6,dv_flow_en=2`.
The host RX, unicast MACs, and 1414-byte UDP payload are the same,
but sender pacing and sampling fraction differ. The benchmark runner
starts the SoC pipeline, then host RX, then TX. Rates average samples
8–23; SoC misses cover the whole trial.

| Sender `-t` | Fraction | Encrypted input Gib/s | Clear host RX Gib/s | SoC RX misses |
|---:|---:|---:|---:|---:|
| 1500 | 0.0 | 192.221 | 185.297 | 134,587 |
| 1550 | 1.0 | 189.779 | 183.012 | 9,344 |

These are two operating points, **not a matched sampling comparison**:
`-t` changes the offered load as well as the sampling fraction. Both
runs had zero software TX drops, replay drops, host RX misses, and
host RX no-mbuf events. The fraction-0.0 run showed zero monitor-output
traffic; the fraction-1.0 run showed about 8.4 Mpps per port in the
monitor-output pipe while active. That pipe counter does not by itself
verify cloned packet contents or delivery to the SF interfaces.

Reproduce with `sweep_sf0`/`sweep_sf1` in
`RUN_ARM_ANTI_REPLAY_SAMPLING_CLONING.md`. The retained TX/RX/SoC
log triplets have prefixes
`benchmarks/decap-isolate-sweep_sf0-1500-sfcompare0d-static-` and
`benchmarks/decap-isolate-sweep_sf1-1550-sfcompare1g-static-`.

## Key result

At fraction 1.0, four ARM workers deliver over 180 Gib/s with cloning, ESP decapsulation,
and software anti-replay enabled. The summary script reports **183.011821
Gib/s** clear-host RX.

The Scenario 3 comparison rate is **87.59046875 Gib/s**,
with reported throughput losses of 69.0% without sampling and up to 70.2% with
sampling when ESP anti-replay is enabled. The numerical throughput ratio is
about **2.09×**. Both experiments use a 1500-byte link MTU, dual-port
DPDK application without link aggregation, hardware setup, and receiver.
See the [experiment README](../README.md) for the
reproduction command and comparison limits. The 189.735 Gib/s four-worker
baseline has monitoring disabled and is a separate result.
