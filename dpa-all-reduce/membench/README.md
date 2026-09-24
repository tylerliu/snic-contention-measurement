# DPA memory benchmark

Measures reads or writes to ARM memory (default) or DPA private memory (`--use_private`). Requires `libgflags-dev` for the container OS. Arguments include `--device`, `--threads`, `--mem_size` (bytes per thread), `--loop`, `--stride` (64-bit elements), `--op read|write`, and `--continuous`. Device throughput assumes a 1.8 GHz clock; see the reproduction guide for result-collection limitations.

Follow the [reproduction guide](../README.md) for DOCA 3.1 container setup, builds, launch commands, correctness checks, firmware/runtime troubleshooting, and measurement limits. Each FlexIO project builds its own `build-doca31/host/flexio_packet_processor`. Native execution requires matching SDK libraries; use the shared [DOCA 3.1 Docker setup](../../README.md#doca-31-docker-setup) for both build and runtime.
