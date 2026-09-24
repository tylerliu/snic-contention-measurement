# ESP RSS multi-worker benchmark

DOCA 3.1, branch DPDK-decap-only. Both PF0 and PF1 forward clear packets to their
host representors after hardware decryption, software decapsulation and replay.

## Results

Two 30-second runs, identical generator settings, 64 flows/SAs per port
(128 total), 1414-byte UDP payload, 1510-byte ESP Ethernet input / 1456-byte
clear Ethernet output, excluding FCS. Rates are aggregate across both host ports,
averaged over the final 15 one-second generator RX samples.

| ARM workers | Host RX Mpps | Clear Ethernet Gib/s | UDP payload Gib/s | CPU sample |
| --- | --- | --- | --- | --- |
| 4 | 5.531 | 60.00 | 58.27 | 400.93% |
| 8 | 5.709 | 61.93 | 60.15 | 801.32% |

Both kept pace with offered generator traffic. These runs do not establish
a maximum throughput ceiling or demonstrate a meaningful scaling gain from
doubling workers. Four workers are sufficient for this measured offered load.

RSS flow counts by queue:
- 4 workers: 33, 31, 33, 31.
- 8 workers: 17, 16, 17, 16, 16, 15, 16, 15.

All 128 flows stayed on one queue each during both benchmarks.
Distribution was balanced enough that increasing the flow count was unnecessary.

4-worker accepted/TX total: 156,238,528.
8-worker accepted/TX total: 161,405,056.
Both: zero malformed/unmarked/replay/TX-drop counters, zero packet linearizations.
Sampled host and SoC RX missed/no-mbuf counters were zero.
Thus the measured packet path used the original RX buffer with no payload copy.

Byte-exact plaintext, duplicate/old/zero replay rejection and bad-tag isolation
tests passed on the first and last SPI of each port's range, for both worker counts.
The same sequence numbers were independently accepted on different SAs.
Unit parser/replay tests pass in the DOCA 3.1 ARM build.

## Implementation

The authentication-success pipe uses DOCA_FLOW_RSS_ESP and all worker queues;
there is NO modulo-based or explicit SPI-to-queue assignment.
Workers poll/Tx their own queue on each PF.
One hardware SA and one software replay window are provisioned per SPI.
The authenticated marker identifies the port range; SPI selects its SA in O(1).
A per-SA mutex remains for correctness if different outer tuples under one SA
hash to different queues. No global replay lock exists.
Each worker logs per-SA received counts and linearization counts at shutdown.

ipsec-decap-flow-count accepts 1..128 (default 1). It expands each configured
base SPI into consecutive SAs using that port's key/salt/MAC template.
This is a lab workload convenience, not production SA/key provisioning.
The benchmark generator uses fixed test keys/IV settings.

## Reproduction

Use rss_4workers.yml (EAL 1-5) or rss_8workers.yml (EAL 1-9).
Main lcore is 1; the remaining 4 or 8 lcores are workers.
Port 0 SPI range is 0x2001..0x2040; port 1 is 0x3001..0x3040.

SoC /home/ubuntu/monitoring_decap_only in the DOCA 3.1 container:

    meson compile -C build
    meson test -C build --print-errorlogs
    stdbuf -oL ./build/monitoring_app --config rss_4workers.yml

Generator uses the existing testing_tools/build/dpdk_esp_gen in the
3.1.0-devel-host container:
- Devices/TX+RX: 0000:01:00.0 and 0000:01:00.1.
- TX lcores 0-14 (main 0 excluded); RX lcores 16-23.
- Source/RX IPs 172.16.1.128 and 172.16.2.128.
- Destination/tunnel IPs 172.16.1.129 and 172.16.2.129.
- SPI bases 0x2001 and 0x3001, same test keys/salts as the YAML.
- --flow-count 64 -z 1414 -t 0 -M 131072
- Bound each run with timeout -s INT -k 5 30.

Wait for app initialization before traffic. Restart the SoC app between
correctness/performance runs to reset replay windows. Stop the generator first.
Machine-readable samples and worker totals: rss_benchmark_results.json.
