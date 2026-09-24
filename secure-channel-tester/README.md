# DOCA Secure Channel Tester

This application measures Host–ARM communication using the DOCA Comm Channel (Comch) API. The ARM endpoint runs on the DPU.

## Description

The application establishes a connection between the Host and the ARM endpoint on the DPU.
- ARM acts as the transport **Server**.
- The Host acts as the transport **Client** and selects the workload parameters.

Once connected, both sides exchange messages to measure performance.
- **Producer**: Sends messages.
- **Consumer**: Receives messages.

Build and Host–ARM setup instructions are in the [repository guide](../README.md).

## Usage

Run from `secure-channel-tester/`. Start the ARM server first, then the Host client.

### ARM server
```bash
sudo ./build/doca_secure_channel -p <PCI_ADDRESS> -r <REP_PCI_ADDRESS>
```

### Host client
```bash
sudo ./build/doca_secure_channel -p <PCI_ADDRESS> -n <NUM_MSGS> -s <MSG_SIZE>
```

### Arguments
- `-p, --pci-addr`: DOCA Comch device PCI address (Required).
- `-r, --rep-pci`: DOCA Comch device representor PCI address (Required on DPU).
- `-n, --num-msgs`: Number of messages to be sent (Required on Host if not Continuous).
- `-s, --msg-size`: Message size in bytes (Required on Host).
- `-d, --direction`: `bidirectional` (default), `send`, or `recv`, relative to the Host.
- `-c, --continuous`: Run in continuous mode (Host only).

**Note**: The DPU configuration is controlled by the Host. Arguments `-n` and `-s` are ignored on the DPU side.

## Output

The application reports the following statistics upon completion:
- **Total Messages**: Number of messages sent/received.
- **Duration**: Total time taken for the operation.
- **Throughput**: Measured in Messages per Second (Msgs/s).
- **Bandwidth**: Measured in MiB/s (printed as `MB/s`).

Bandwidth is computed in MiB/s (1024² bytes/s), although the log label is `MB/s`.

### Example Output (illustrative; not measured reference data)
```
INFO: Producer sent 10000 messages in approximately 50.0000 milliseconds
INFO: Producer Throughput: 200000.00 Msgs/s
INFO: Producer Bandwidth: 195.31 MB/s
INFO: Consumer sent 10000 messages in approximately 50.0000 milliseconds
INFO: Consumer Throughput: 200000.00 Msgs/s
INFO: Consumer Bandwidth: 195.31 MB/s
```
