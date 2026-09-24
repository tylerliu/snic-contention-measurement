# Throughput Experiment

A DPDK-based benchmarking suite for evaluating the performance and throughput of `cryptodev` and `compressdev` libraries. This project supports both sequential and concurrent benchmarking with highly configurable test cases and automated orchestration.

## Features

- **Single-Process Benchmarking**: Orchestrate one or more benchmark configurations (e.g., varying data sizes or algorithms) in isolation using `run_benchmarks.py`.
- **Concurrent Benchmarking**: Execute multiple DPDK processes simultaneously on different CPU cores to simulate multi-tenant or contested environments using `run_concurrent_throughput.py`.
- **JSON-Driven Configuration**: Define test cases, EAL arguments, and benchmark parameters in `benchmark_cases.json` and `concurrent_scenarios.json`.
- **Performance Optimizations**:
  - CPU pinning via `taskset`.
  - CPU governor and idle state tuning for consistent measurements.
  - Warm-up cycles to ensure stable performance before measurement.
- **Automated Data Generation**: Tooling to generate compressed test data for compression benchmarks.
- **Result Analysis**: Automatic CSV reporting and detailed logging of stdout/stderr for every run.

## Prerequisites

- **DPDK**: Development headers and libraries installed (must be discoverable by `pkg-config`).
- **Python 3**: For orchestration scripts.
- **Build System**: Meson and Ninja.
- **System**: Linux with root/sudo access (required for DPDK device initialization and CPU tuning).

## Getting Started

### 1. Build the Project

```bash
meson setup build
ninja -C build
```

### 2. Generate Test Data (for Compression)

If you plan to run compression benchmarks, generate the necessary test data first:

```bash
python3 generate_compressed_data.py --output-dir compressed_data
```

### 3. Run Single-Process Benchmarks

Execute all benchmarks defined in `benchmark_cases.json`:

```bash
sudo python3 run_benchmarks.py
```

Or run a specific benchmark:

```bash
sudo python3 run_benchmarks.py rte_cryptodev_enqueue_dequeue_burst_encrypt
```

### 4. Run Concurrent Scenarios

Execute complex concurrent workloads defined in a scenario file:

```bash
sudo python3 run_concurrent_throughput.py --scenario concurrent_scenarios.json
```

## Configuration

### `benchmark_cases.json`
Defines the base benchmark configurations, including:
- `eal_args`: DPDK EAL arguments (e.g., device PCI addresses).
- `params`: Parameters passed to the benchmark executable (e.g., `burst_size`, `data_size`).
- `grouped_params`: Combinations of parameters to run for each case.
- `setup_command`: Optional command to run before the benchmark (e.g., data generation).

### `concurrent_scenarios.json`
Defines groups of jobs to be run concurrently:
- `groups`: Logical sets of concurrent jobs.
- `shared_params`: Parameters shared across all jobs in a group.
- `jobs`: Individual DPDK processes to launch, specifying the core, multiplier, and job-specific overrides.

## Directory Structure

- `benchmarks/`: C source files for specific DPDK API benchmarks.
- `driver/`: Shared benchmark driver code for timing and DPDK initialization.
- `bench_utils.py`: Shared Python utilities for performance tuning and result parsing.
- `run_benchmarks.py`: Orchestrator for single-process benchmark runs.
- `run_concurrent_throughput.py`: Orchestrator for concurrent DPDK process runs.
- `generate_compressed_data.py`: Utility for creating test datasets for `compressdev`.
- `build/`: Output directory for compiled executables (created by Meson).

## Measurement Methodology

The benchmarks use the CPU's Time Stamp Counter (TSC) for precise timing.
1. **Setup**: CPU governor is set to `performance`, and idle states are disabled.
2. **Warm-up**: The benchmark is run briefly to prime caches and stabilize frequencies.
3. **Execution**: The benchmark runs for a specified number of iterations.
4. **Parsing**: The script parses the TSC cycles from the output and calculates:
   - `cycles_total`: Total cycles consumed.
   - `cycles_per_op`: Average cycles per operation.
   - `ops_per_sec`: Throughput based on the detected TSC frequency.
