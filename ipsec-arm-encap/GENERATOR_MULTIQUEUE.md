# Per-SA TX queue generator

Generator changed in ../partial_decrypt/testing_tools/dpdk_esp_gen.c; existing local
flow-count support preserved. Built with DOCA 3.1 devel-host.

Each active ESP SA has one data TX queue and a private increasing sequence counter.
Each queue has exactly one worker owner; a worker may service several queues.
64-packet batches stay within one flow. Removed shared-queue token ring.
Fixed partial burst pointer advancement, partially filled batch indexing, and
unsent-mbuf cleanup at shutdown. Workers stop before ESP sequence wrap.
Main ARP and RX-worker ARP use separate queues from data traffic.

Data queues/active SAs are capped by the minimum reported TX capacity across TX
ports after reserving control queues. These devices report max_tx_queues=1024;
five reserved queues leave 1019 data queues. The configured 64 SAs per port
therefore use 64 data queues plus five control queues, 69 total per port.
If capped, the generator explicitly reports reduced active SAs.
The actual hardware cap branch was not exercised on this device.

## Bounded benchmark

Eight ARM workers, DOCA 3.1, dual PF, 64 SAs/port, 128 total.
Encrypted Ethernet frame 1510 bytes; decrypted Ethernet frame 1456 bytes.
30-second runs; final 15 one-second RX samples averaged.

| TX layout | Generator TX workers | Host RX Mpps | Clear Ethernet Gib/s |
| --- | --- | --- | --- |
| Previous shared queue | 14 | 5.7091 | 61.93 |
| One SA per queue | 14 | 9.2794 | 100.66 |
| One SA per queue, SMT | 30 | 7.1886 | 77.98 |

14 TX workers: lcores 1-14; main 0. 30 TX workers: 1-15,25-39.
Host RX lcores 16-23 in both. ARM worker configuration unchanged.
14-worker gain versus previous shared queue is approximately 62.5%.
30 workers use SMT siblings and were slower; the cause has not been profiled.
Host TX and RX rates matched within sampling skew. All eight ARM final totals
in both runs report zero malformed, unauthenticated, replay, and TX-drop packets.
Host sampled missed/no-mbuf were zero.
Generator partial retry counters were zero, so the corrected partial-send branch
was not exercised by these runs. Shutdown discarded only pending unsubmitted mbufs.
No maximum receiver capacity claim is made.

Raw logs: generator_multiqueue_14.log and generator_multiqueue_30.log.
