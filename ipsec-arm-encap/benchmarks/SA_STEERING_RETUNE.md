# SA-steered four-worker decrypt retune

2026-09-23, DOCA 3.1. Six pacing points plus one repeat, using 16 ESP TX
workers (eight SAs per port), four SoC workers, and two host RX workers.
Each authenticated SA is steered to queue `SA index % 4`; the software
256-packet replay window remains per SA. Sender uses static unicast MACs,
1414-byte UDP payloads, and the existing
`benchmarks/run_decrypt_isolation.sh sweep PAUSE sabalPAUSE 16 static`
procedure. TX runs 30 seconds; host RX runs 45 seconds. Rates below are
means of samples 8–23, independently for TX and RX. Misses cover the
whole SoC trial.

| TX `-t` | Encrypted input Gib/s | Clear host RX Gib/s | SoC RX misses |
|---:|---:|---:|---:|
| 2100 | 168.086 | 162.084 | 0 |
| 1600 | 187.867 | 181.127 | 0 |
| 1400 | 196.801 | **189.785** | 0 |
| 1400 repeat | 196.711 | **189.685** | 0 |
| 1350 | 198.849 | 189.537 | 6,590,751 |
| 1300 | 201.915 | 188.166 | 17,478,055 |
| 1200 | 207.152 | 183.018 | 44,642,627 |

The best tested setting is `-t 1400`: the two runs average **189.735
Gib/s** clear host RX with zero reported SoC RX misses. The previous
four-worker unicast sweep peaked at 179.276 Gib/s at `-t 1400`, with
26,868,941 misses. This is a 10.46 Gib/s higher delivered rate in these
single-run comparisons, not a controlled confidence interval.

All 16 active SAs appeared on their assigned queue only, four SAs per
worker. At `-t 1400`, worker accepted-packet totals were 127,165,952,
126,834,368, 126,947,840, and 126,927,232 (max–min 0.26% of their
mean). On the repeat they were 126,746,688, 126,937,408, 126,275,776,
and 126,826,176 (max–min 0.52% of their mean). Every run reported zero
software TX drops, replay drops, host RX misses, and host RX no-mbuf
events. The generator itself has one SA per TX worker, so this measures
16 active SAs rather than the configured capacity of 64 per port.

Raw TX/RX/SoC logs are
`benchmarks/decap-isolate-sweep-PAUSE-sabalPAUSE-static-{tx,rx,soc}.log`.
The repeat uses `sabal1400b`. The raw logs are retained separately from
the older four-worker sweep.
