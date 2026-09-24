# SA-steered single-worker decrypt retune

2026-09-23, DOCA 3.1, 16 ESP TX workers (eight SAs per port), one
SoC DPDK packet worker, two host RX workers. The SoC performs zero-copy
decapsulation and 256-packet per-SA anti-replay after hardware ESP
authentication/decryption. The current per-SA steering has one destination
queue when there is one worker. The per-SA replay mutex has been removed.
Sender uses explicit unicast destination MACs and 1414-byte UDP payloads.

Each trial used
`bash benchmarks/run_decrypt_isolation.sh sweep1 PAUSE saonePAUSEa 16 static`
except the second 7200 run, labeled `saone7200b`. Sender duration is
30 seconds; host RX duration is 45 seconds. Rates below average interval
samples 8–23; misses are cumulative over the entire SoC trial.
Rates are aggregate over both ports.

| Sender `-t` | Encrypted input Gib/s | Clear host RX Gib/s | Host RX Mpps | SoC RX misses |
|---:|---:|---:|---:|---:|
| 9250 | 61.538 | 59.335 | 5.470 | 0 |
| 9000 | 62.996 | 60.742 | 5.599 | 0 |
| 8500 | 65.993 | 63.632 | 5.866 | 0 |
| 8000 | 69.295 | 66.817 | 6.159 | 0 |
| 7800 | 70.458 | **67.939** | 6.263 | 0 |
| 7500 | 72.920 | 70.090 | 6.461 | 347,797 |
| 7300 | 74.551 | 71.884 | 6.626 | 68,883 |
| 7200 a | 75.355 | **72.635** | 6.696 | 144,873 |
| 7200 b | 75.296 | **72.227** | 6.658 | 1,392,695 |
| 7100 | 76.205 | 70.914 | 6.537 | 6,873,292 |
| 7000 | 77.027 | 70.950 | 6.540 | 8,923,420 |
| 6800 | 78.830 | 70.662 | 6.514 | 14,204,433 |
| 6000 | 86.716 | 70.226 | 6.474 | 35,807,804 |
| 5000 | 99.207 | 69.593 | 6.415 | 69,307,196 |

Best tested delivered throughput is `-t 7200`, averaging **72.431353
Gib/s** over two runs. It is not lossless. The fastest tested point with
zero reported SoC RX misses is `-t 7800` at **67.938572 Gib/s**.
The earlier one-worker sweep peaked at 59.300458 Gib/s at `-t 9250`.
This retune changes both the DOCA steering layout and the software replay
synchronization, so the results do not isolate either cause.

Every run reported zero software TX drops, replay drops, host RX misses,
and host RX no-mbuf events. The original source logs for non-peak trials
were removed after their rates were recorded here. Retained raw logs are
only the TX/RX/SoC triplets for `saone7200a`, `saone7200b`, and
`saone7800a`, named
`benchmarks/decap-isolate-sweep1-PAUSE-LABEL-static-{tx,rx,soc}.log`.
