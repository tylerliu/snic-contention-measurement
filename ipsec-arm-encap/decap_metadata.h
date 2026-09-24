#ifndef DECAP_METADATA_H
#define DECAP_METADATA_H
#include <cstddef>
#include <cstdint>
inline uint32_t decap_authenticated_mark(size_t pair) { return 0xdec00000u + uint32_t(pair) + 1; }
#endif
