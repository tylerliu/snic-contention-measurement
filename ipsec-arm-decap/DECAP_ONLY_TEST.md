# DOCA decrypt + DPDK decap/replay/host forwarding

The decapsulation correctness checks were recorded with DOCA 3.1.0105,
DPDK 22.11.2507.1.0, release (-O3), in
nvcr.io/nvidia/doca/doca:3.1.0-devel on the SoC.

## Pipeline

ESP -> DOCA AES-256-GCM decrypt/authenticate -> syndrome==0 and RX marker
-> ESP RSS across worker queues -> DPDK validate plaintext/trailer/SPI -> per-SA 256-packet
anti-replay window -> in-place decapsulation/L2 rewrite -> batched ARM TX
-> per-PF EGRESS root -> matching host representor.

No DPDK cryptodev work and no CPU decryption. DOCA performs no decapsulation.
Only the successful authentication pipe supplies the expected RX metadata.
DOCA pkt_meta must be encoded in big-endian for the expected DPDK metadata value.
Malformed, unmarked, duplicate, too-old and sequence-zero packets are discarded.
Replay state is updated after authentication and plaintext validation, before TX;
a packet dropped under TX congestion cannot be replayed.
ipsec-decap-anti-replay controls software replay; hardware sequence checking is off.
Input VLAN envelopes are parsed; output Ethernet is deliberately untagged.
Normal single-segment packets reuse the RX mbuf without a payload copy;
multi-segment packets are linearized first.

Current scope: IPv4 ESP tunnel, AES-256-GCM, 16-byte ICV, no ESN,
1..128 consecutive SPIs per monitor/host pair, with a hardware SA and software
replay window per SPI. ipsec-decap-flow-count defaults to 1. Queue count follows
the number of EAL worker lcores. Each authenticated SA is steered to the worker queue selected by its flow index
modulo the worker count. Each SA's software replay state belongs to that worker;
no per-SA replay mutex is used. Metadata/SPI lookup is O(1).

PF0 and PF1 have separate switches (esw_multiport=false). All flow creation,
SA binding, entry processing and EGRESS rules use each PF's switch context.
The original first-switch EGRESS lookup was insufficient for PF1.
ESP dispatch is scoped to each switch and its configured SPI, not exclusively
to the physical uplink port ID: host-injected test traffic can carry another
logical source port. Authentication and replay checks still gate all output.
No NIC, OVS or e-switch mode settings were changed.

## Verified correctness

test_decap_host.py injects ten frames per port with correct IPv4 checksums.
Run with fresh SA state after the SoC app reports initialized, and without
a DPDK generator owning host RX steering.

Accepted sequences: 1, 4, 3, 300, 301.
Rejected in software: duplicate 1, duplicate 3, old 2, zero.
Hardware rejects a corrupted-tag 301; subsequent valid 301 still succeeds.
The test compares every inner-IP/UDP/payload byte, plus destination MAC.

Both PF0/ens1f0np0 and PF1/ens1f1np1 passed in one running application:
10 accepted/TX, 8 replay rejects, 2 hardware authentication drops,
zero malformed/unmarked/TX drops. Each EGRESS entry counted five packets.
PF1 also passed independently.

Parser and replay Meson tests pass in the DOCA 3.1 ARM container.
Local ASan/UBSan tests cover truncation, padding, next-header/inner-length errors,
fragmentation, VLAN parsing, replay boundaries, large jumps, and sequence wrap.

## Reproduction

SoC source/build: /home/ubuntu/monitoring_decap_only (separate from master deployment).

Inside the privileged DOCA 3.1 container with host networking and
/dev/hugepages, /dev/infiniband and the source directory mounted:

    meson setup build --buildtype=release
    meson compile -C build
    meson test -C build --print-errorlogs
    stdbuf -oL ./build/monitoring_app --config phase1_decap_only_dual.yml

Configs: phase1_decap_only.yml (PF0), phase1_decap_only_pf1.yml (PF1),
phase1_decap_only_dual.yml (both). All use main lcore 1, worker lcore 2,
and a dummy initial PCI allowlist so DOCA owns PF probing.
No lookaside crypto device is required.

After startup, from `ipsec-arm-decap/` on the host with the Python
`cryptography` package installed:

    sudo python3 test_decap_host.py
    sudo python3 test_decap_host.py --interface ens1f1np1 --spi 0x2002 --host-mac 58:a2:e1:53:19:d7

Tests use fixed lab keys. Restart the SoC application to reset replay state
before repeating the same sequence numbers. Stop traffic before shutdown.

Initial layout validation (before forwarding) parsed 5,408,000 packets with
no parser errors. For the 1510-byte frame: outer IPv4 starts at byte 14,
ESP at 34, IV at 42, plaintext inner IPv4 at 50 (length 1442),
trailer at 1492, ICV in the final 16 bytes.
