# DPA receive-only control

Receives IPv4/UDP packets and reports input packet and byte rates. Use `-p` for UDP destination port (default 1234), `-t` for DPA threads, `-a` for ARM-backed receive buffers, and `-v` for verbose output. Receive data storage is 128 entries × 2048 bytes = 256 KiB per thread. At 256 threads, supplying traffic must exercise all 256 source-port buckets. Use power-of-two thread counts with matching source-port distribution.

Follow the [reproduction guide](../README.md) for DOCA 3.1 container setup, builds, launch commands, correctness checks, firmware/runtime troubleshooting, and measurement limits. Each FlexIO project builds its own `build-doca31/host/flexio_packet_processor`. Native execution requires matching SDK libraries; use the shared [DOCA 3.1 Docker setup](../../README.md#doca-31-docker-setup) for both build and runtime.
