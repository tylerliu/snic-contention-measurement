# ARM TX retry-until-sent — 2026-09-22

Implemented in normal monitoring_dpdk.cpp, source commit `6b2278c`.
For every ready burst, repeatedly call rte_eth_tx_burst on the unsent suffix.
Advance the pointer/count only by the returned accepted count; never resend or
free packets already owned by the PMD. Call rte_pause after any incomplete
attempt. No retry count or time limit while running. Check the worker shutdown
flag each iteration; on shutdown free only the remaining unsent suffix.

This changes the default decrypt worker behavior. It does not change the separate
net_arm_forward executable, whose bounded --retries experiment remains available.
While retrying, that ARM worker does not poll either RX port, so RX overflow is
possible. `tx_drop` can still count abandoned packets during shutdown (and the
existing optional fresh-buffer diagnostic allocation failures).

## Test

DOCA 3.1 release, normal build with no diagnostic flags; layout and replay unit
tests both passed. Physical 82:00.* → ARM → host 01:00.*, 16 unthrottled ESP
generator workers, 16 SAs, four ARM workers, two host RX workers. Same
decrypt_16tx_4arm.yml and payload 1414 (1510-byte encrypted input / 1456-byte output).
Sender 30 seconds, receiver 45 seconds; ARM safety timeout 100 seconds, stopped
after collection. App shutdown returned 0; sender/receiver timeouts returned124.

Both throughput numbers below use fixed host RX samples 8–23, not peak filtering:

| Policy | Host Mpps | Host Gib/s | Software TX drops | SoC RX missed |
|---|---:|---:|---:|---:|
| Previous single attempt | 1.117004 | 12.117309 | 411,377,325 | 35,242,492 |
| Retry until accepted/shutdown | 1.114090 | 12.085700 | 0 | 446,530,030 |

Retry-until-sent accepted and transmitted 31,406,418 packets. Hardware egress
counters sum to the same total: 15,643,753 + 15,762,665. Software malformed,
unauthenticated-marker and replay counters are zero. Host RX misses/no_mbuf
are zero. SoC RX misses are 223,322,071 + 223,207,959, with no_mbuf/errors zero.

**No throughput recovery:** loss moved from software TX drops to ARM RX misses.
This is a heavily lossy overload trial, not a lossless throughput result. The
worker now fulfills retry-until-sent semantics, but does not make the NIC accept
TX faster. Root cause of the underlying congestion remains unresolved.

Reproduce, after deploying/rebuilding the normal application:

```sh
bash benchmarks/run_decrypt_isolation.sh until-sent
```

Logs: `decap-isolate-until-sent-{soc,tx,rx}.log`.
Previous baseline: `decap-isolate-four-arm-{soc,tx,rx}.log`.
All test containers stopped and removed. Unique host test mappings cleaned;
both 4 GiB reservations preserved, generic SoC rtemap_0 left untouched.
