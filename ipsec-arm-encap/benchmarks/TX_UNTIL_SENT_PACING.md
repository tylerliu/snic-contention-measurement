# Retry-until-sent with sender pacing

2026-09-22. Same DOCA 3.1 normal release binary with ARM retry-until-sent
(`6b2278c`), physical 82:00.* → ARM → host 01:00.*; 16 generator workers,
four ARM workers, two host RX workers. Config decrypt_16tx_4arm.yml.
Only generator `-t` changes. Each sender worker executes that many rte_pause
calls per batch of up to 64 packets; this is not calibrated PPS pacing.

| Sender throttle | Encrypted TX Mpps | Encrypted TX Gib/s | Clear host RX Mpps | Clear host RX Gib/s |
|---:|---:|---:|---:|---:|
| 5000 | 8.811248 | 99.129859 | 8.810062 | 95.571955 |
| 4000 | 10.283108 | 115.688838 | 10.283406 | 111.554859 |

All rates are fixed samples 8–23 of each respective stream. Small TX/RX PPS
differences reflect interval alignment. ESP input frames are 1510 bytes and
clear output frames 1456 bytes, excluding FCS; Gib/s = pps * bytes * 8 / 2^30.
Each trial: sender 30 seconds, host receiver 45 seconds; SoC stopped afterward
with a 100-second safety timeout. Normal app shutdown returned zero.

Final ARM accepted = transmitted:
- 5000: 246,684,480; hardware egress = 123,377,024 + 123,307,456.
- 4000: 288,125,312; hardware egress = 144,085,376 + 144,039,936.

Both trials: zero software TX drops, malformed, unauthenticated-marker and replay
drops; zero SoC RX misses/errors/no_mbuf; zero host RX misses/no_mbuf. These are
single bounded trials with matching counters, not sequence-based proof of
losslessness or a measured maximum sustainable rate. The 4000 trial delivers
about 16.7% more clear throughput than 5000. The earlier single-attempt 5000 run
was 95.61 Gib/s, so these data do not show a retry-related gain at that load.

Source runner revision: `1cde470`; generator checkpoint: `2b2f873`.
ARM binary SHA256:
`4313f187563e974f18265e585058a9dfaeb2125d35f71e3db3383d60670ae7f1`.

```sh
bash benchmarks/run_decrypt_isolation.sh until-sent-5000
bash benchmarks/run_decrypt_isolation.sh until-sent-4000
```

Logs: `decap-isolate-until-sent-{5000,4000}-{soc,tx,rx}.log`.
All containers stopped/removed; unique idle host mappings cleaned. Both 4 GiB
hugepage reservations preserved; generic SoC rtemap_0 left untouched.
