#ifndef LITEFS_DISK_RING_H
#define LITEFS_DISK_RING_H

#include <cstdint>
#include <cstddef>

/**
 * DiskArea layout (host memory exported for NIC DMA):
 * [ RingBytes[DISK_RING_REGION_SIZE] | FS persistence region ... ]
 * NIC writes logs into RingBytes via DMA.
 * Host consumes logs from RingBytes.
 */

// Sizes and offsets within DiskArea
static constexpr size_t kLogAreaSize = 256 * 1024 * 32; // 8MB ring buffer per application/generator
// Default ring region: match log area size to keep chunk math simple (16 MiB) - 2x log area size
static constexpr size_t DISK_RING_REGION_SIZE = kLogAreaSize * 2; // 2x log area size
static constexpr size_t kDiskAreaSize = 1ULL << 30; // 1 GiB

// FS region starts immediately after the ring region
static constexpr size_t DISK_FS_BASE_OFFSET = DISK_RING_REGION_SIZE;

#endif // LITEFS_DISK_RING_H


