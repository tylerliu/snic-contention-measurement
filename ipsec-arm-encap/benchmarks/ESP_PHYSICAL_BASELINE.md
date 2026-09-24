# Physical ESP generator baseline (2026-09-19)

Corrected topology: ConnectX-7 0000:82:00.0/.1 in the existing remote network
namespace sends across physical links to BlueField uplinks; existing OVS NORMAL
forwarding delivers encrypted ESP to host 0000:01:00.0/.1.
No DOCA application, decryption, decapsulation, or replay processing.
OVS rules, namespaces, port assignments, and interface MTUs were not modified.

DOCA 3.1 devel-host container binaries. dpdk_simple_recv -p 0 receives and frees
ESP; despite supplying lcores 43-47, this source launches only two RX workers,
one queue per port (44,45). Main receiver core is 43.
Generator is TX-only. Sender container joins /var/run/netns/remote via nsenter
and mounts its own sysfs view so RDMA devices in that namespace are visible.
First attempted runs failed before traffic because build/ binaries were missing;
both tools were rebuilt.

Payload 1414 bytes, ESP Ethernet frame 1510 bytes excluding FCS.
User's historical payload 1472 produces 1570-byte ESP frames; not tested because
SoC p0/p1 MTU is 1500. No MTUs were changed.

| Generator | TX workers | SAs per port | Host RX Mpps | Encrypted Ethernet Gib/s |
| --- | --- | --- | --- | --- |
| Current multi-queue | 42 | 64 | 7.4346 | 83.64 |
| Original shared queue | 42 | 1 | 6.4215 | 72.24 |
| Current multi-queue | 14 | 64 | 6.6909 | 75.27 |

These are bounded 30-second sender runs with a 45-second independent receiver.
Rates average the last 15 steady active RX samples, excluding startup/shutdown
partial intervals and the receiver's idle tail. For reproducibility, sample
selection was RX >7 Mpps for the first run and >6 Mpps for the other two.
Raw TX/RX interval logs are adjacent to this file.
TX and RX steady rates closely matched; reported RX missed/no_mbuf stayed zero.
Current generator reported zero partial-send retries.
Single runs, not a repeated statistical benchmark or proof of maximum throughput.

Original generator source was read from partial_decrypt HEAD commit 2900b6e,
saved as esp_gen_shared_reference.c and compiled separately against dpdk_common.c.
Current generator changes were NOT reverted. Shared reference retains its old
sequence/token and partial-send behavior, so it is only a historical comparison.
The shared and multi-queue runs differ in active SA count as noted above.

The earlier host-PF-to-same-host-PF DOCA results are a DIFFERENT path, not a valid
direct comparison to this physical-link baseline. The actual namespace/path was
not used in those earlier tests.

Sender arguments (current):
-a 0000:82:00.0 -s 172.16.1.20 -d 172.16.1.128 -T 172.16.1.2 --spi 0x2001 --key aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899 --salt 0x22334455 --iv 0x1234567890abcdef -a 0000:82:00.1 -s 172.16.2.20 -d 172.16.2.128 -T 172.16.2.2 --spi 0x2002 --key 99887766554433221100ffeeddccbbaa99887766554433221100ffeeddccbbaa --salt 0x66778899 --iv 0xfedcba0987654321 -l 0-42 -z 1414 -p 3333 -S -f esp-speed-tx -M 262144 --flow-count 64

For 14 TX workers substitute -l 0-14. Shared reference omits --flow-count.
Receiver arguments:
-a 0000:01:00.0 -a 0000:01:00.1 -l 43-47 -p 0 -f esp-speed-rx
-M 65536 --rx-ip 172.16.1.128 --rx-ip 172.16.2.128 -S

All test processes were stopped and test hugepage reservations cleaned afterward.
