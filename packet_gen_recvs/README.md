# Testing Tools

This directory contains various packet generation and testing tools.

## List of Tools

- `dpdk_simple_gen`: DPDK-based simple packet generator
- `dpdk_aes_gcm_gen`: DPDK-based AES-GCM encrypted packet generator
- `dpdk_esp_gen`: ESP generator with the original shared-SA/ordered-sequence TX path
- `dpdk_esp_exclusive_gen`: ESP generator with one queue and SA per TX worker
- `pkt_gen`: Simple packet generator
- `pkt_recv`: Packet receiver
- `aes_gcm_pkt_gen`: Socket-based AES-GCM packet generator 

## DPDK ESP Generators

Both ESP programs use a private OpenSSL library/cipher context per TX worker.
`dpdk_esp_gen` preserves the original shared-SA, shared-queue sequencing path.
`dpdk_esp_exclusive_gen` gives each TX worker one data queue and one SA;
only this executable accepts `--flow-count`. Use the DOCA 3.1 host image
to build both targets. Packet-validation source is included as `esp_private_crypto_test.c`.
Historical `ESP_EXCLUSIVE_*.md`, `ESP_PRIVATE_*.md`, and `ESP_CPU_PROFILE.md`
reports are not included in this checkout. See the [repository guide](../README.md)
and [Discussion experiment](../ipsec-arm-encap/README.md) for retained results.

## DPDK Simple Packet Generator

A high-performance fixed-size packet generator using DPDK. This tool generates packets with configurable payload sizes and simple pattern data, without encryption overhead. **Supports multiple ports/NICs for increased throughput.**

### Prerequisites

- DPDK development libraries (Mellanox DPDK installed in `/opt/mellanox/dpdk`)
- Meson build system

### Building

Build with meson:

```bash
meson build -Dpkg_config_path=/opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig
ninja -C build
```

### Usage

```bash
# Basic usage (single port)
sudo ./build/dpdk_simple_gen -a <DEVICE> -l <LCORES> -d <DEST_IP>

# Multi-port usage
sudo ./build/dpdk_simple_gen -a <DEVICE1> -a <DEVICE2> -l <LCORES> -d <DEST_IP>

# Examples
sudo ./build/dpdk_simple_gen -a 0000:01:00.0 -l 0-3 -d 192.168.1.100
sudo ./build/dpdk_simple_gen -a 0000:01:00.0 -a 0000:01:00.1 -l 0-3 -d 192.168.1.100
sudo ./build/dpdk_simple_gen -a 0000:01:00.0 -a 0000:01:00.1 -l 0-3 -z 512 -p 3282 -d 192.168.1.100
```

### Command Line Options

- `-a, --device DEVICE`: DPDK device to bind to (e.g., `0000:01:00.0`). Can be specified multiple times for multi-port operation.
- `-l, --lcores LCORES`: Logical cores to use (e.g., `0-3`, `0,2,4`)
- `-p, --port PORT`: Destination port (default: 3282)
- `-s, --src-ip IP`: Source IP address (default: 10.0.0.1)
- `-d, --tx-dst-ip IP`: Destination IP address (repeat per TX port).
- `-z, --size SIZE`: Payload size in bytes (max: 1472)
- `-h, --help`: Show help message

### Features

- **Fixed-size packets**: All packets have the same payload size (configurable)
- **High performance**: Multi-core packet generation using DPDK
- **Multi-port support**: Automatically distributes traffic across multiple ports/NICs for increased throughput
- **Equal capacity distribution**: Assumes equal capacity across all ports and distributes queues evenly
- **Simple pattern data**: Payload filled with repeating byte pattern
- **No encryption overhead**: Faster than encrypted generators
- **Configurable payload size**: Test different packet sizes for performance analysis

## DPDK AES-GCM Packet Generator

A high-performance AES-GCM encrypted packet generator using DPDK. **Supports multiple ports/NICs for increased throughput.**

### Prerequisites

- DPDK development libraries (Mellanox DPDK installed in `/opt/mellanox/dpdk`)
- OpenSSL development libraries
- Meson build system

### Building

Build with meson:

```bash
meson build -Dpkg_config_path=/opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig
ninja -C build
```

### Usage

```bash
# Basic usage (single port)
sudo ./build/dpdk_aes_gcm_gen -a <DEVICE> -p <PORT> <DEST_IP>

# Multi-port usage
sudo ./build/dpdk_aes_gcm_gen -a <DEVICE1> -a <DEVICE2> -p <PORT> <DEST_IP>

# Examples
sudo ./build/dpdk_aes_gcm_gen -a 0000:01:00.0 -p 3282 192.168.1.100
sudo ./build/dpdk_aes_gcm_gen -a 0000:01:00.0 -a 0000:01:00.1 -p 3282 192.168.1.100
sudo ./build/dpdk_aes_gcm_gen -a 0000:01:00.0 -a 0000:01:00.1 -p 3282 -s 192.168.1.10 192.168.1.100
```

### Command Line Options

- `-a, --device DEVICE`: DPDK device to bind to (e.g., `0000:01:00.0`). Can be specified multiple times for multi-port operation.
- `-p, --port PORT`: Destination port (default: 3282)
- `-s, --src-ip IP`: Source IP address (default: 10.0.0.1)
- `-t, --threads NUM`: Number of worker threads (default: 1)
- `-h, --help`: Show help message

### EAL Arguments

You can also pass standard DPDK EAL arguments:

```bash
sudo ./build/dpdk_aes_gcm_gen -l 0-3 -n 4 -a 0000:01:00.0 -p 3282 192.168.1.100
```

## Multi-Port Operation

Both DPDK packet generators now support multiple ports/NICs for increased throughput. The generators automatically:

- **Detect all available ports**: Uses all DPDK-bound devices
- **Distribute queues evenly**: Splits available queues across all ports
- **Balance traffic**: Worker threads are distributed across ports using modulo arithmetic
- **Handle port-specific MAC addresses**: Each port uses its own MAC address for packet generation

### Multi-Port Architecture

```
Worker Threads: [0, 1, 2, 3, 4, 5, 6, 7]
Ports: [Port 0, Port 1]
Queue Distribution:
- Thread 0 → Port 0, Queue 0
- Thread 1 → Port 1, Queue 0  
- Thread 2 → Port 0, Queue 1
- Thread 3 → Port 1, Queue 1
- Thread 4 → Port 0, Queue 2
- Thread 5 → Port 1, Queue 2
- Thread 6 → Port 0, Queue 3
- Thread 7 → Port 1, Queue 3
```

### Multi-Port Usage Examples

```bash
# Two-port setup with 4 cores
sudo ./build/dpdk_simple_gen -a 0000:01:00.0 -a 0000:01:00.1 -l 0-3 -d 192.168.1.100

# Two-port AES-GCM with custom source IP
sudo ./build/dpdk_aes_gcm_gen -a 0000:01:00.0 -a 0000:01:00.1 -l 0-3 -s 10.0.0.1 192.168.1.100

# Four-port setup (if you have 4 NICs)
sudo ./build/dpdk_simple_gen -a 0000:01:00.0 -a 0000:01:00.1 -a 0000:02:00.0 -a 0000:02:00.1 -l 0-7 -d 192.168.1.100
```

### Performance Considerations

- **Equal capacity assumption**: The generators assume all ports have equal capacity
- **Queue distribution**: Queues are distributed evenly across ports (queues_per_port = total_queues / num_ports)
- **Minimum requirements**: Need at least as many queues as ports
- **Optimal core count**: For best performance, use a number of cores that's a multiple of the number of ports

