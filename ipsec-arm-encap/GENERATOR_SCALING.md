# Generator worker scaling

DOCA 3.1, unchanged eight-worker ARM receiver, dual PF, 64 ESP flows per port,
1414-byte UDP payload, 1510-byte encrypted Ethernet frame, 1456-byte clear frame.
Each new run was capped at 30 seconds; rates average the final 15 one-second samples.
Host RX workers stayed on lcores 16-23. Main lcore 0.

| Generator TX workers | TX lcores including main | TX Mpps | Host RX Mpps | Clear Ethernet Gib/s |
| --- | --- | --- | --- | --- |
| 8 | 0-8 | 5.4973 | 5.4973 | 59.64 |
| 14 (previous run) | 0-14 | 5.7090 | 5.7091 | 61.93 |
| 22 | 0-15,25-31 | 5.9309 | 5.9309 | 64.34 |
| 30 | 0-15,25-39 | 6.3114 | 6.3115 | 68.47 |

The 30-worker configuration uses 15 physical TX cores and their SMT siblings.
It improves host received throughput about 10.6% over the previous 14-worker run.
Sampled host missed/no-mbuf counters were zero. All eight ARM final worker totals
for the 22- and 30-TX-worker runs had zero malformed, unauthenticated, replay,
and TX-drop counts. TX and RX rates match within sampling skew.
These measurements do not establish maximum receiver capacity.

## Generator inspection

Source: ../partial_decrypt/testing_tools/dpdk_esp_gen.c.
No generator or receiver implementation changes were made in this investigation.

- Genuine multi-worker packet construction and OpenSSL encryption, but one shared
  TX queue per port. Workers pass a token in strict rank order every 64 packets.
- Existing build flags include O3/Ofast. The DPDK dependency appends march=corei7
  after march=native; release mode alone is not a missing optimization.
- Each packet reconstructs plaintext and encrypts into a stack ciphertext buffer
  before copying into its mbuf.
- Partial sends at lines 523-526 reduce pending_count without advancing the mbuf
  pointer. Retrying can resubmit already-owned mbufs. This needs fixing before
  overload testing; zero receiver counters do not prove that path is safe.
- Reverse pending-array filling is also unsafe if mbuf allocation stops early.
- Sequence stride adds N*64 after already advancing 64, leaving gaps each round.
- The startup message's reported core field is misleading: its first worker ID
  is the real lcore, while the second field always prints the first worker core.

Next implementation candidate: a TX queue per worker with disjoint SA/flow
ownership and increasing per-SA sequence counters, eliminating cross-worker
token handoff while preserving replay ordering. Correct partial-send ownership
and shutdown behavior first. No such changes were applied here.

Raw generator logs for new runs are in generator_scaling_8.log,
generator_scaling_22.log, and generator_scaling_30.log. Logs for 22/30 also include
all receiver final worker totals; the 8-worker log contains only the receiver tail.
