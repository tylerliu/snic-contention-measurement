#ifndef ESP_LAYOUT_H
#define ESP_LAYOUT_H
#include <cstddef>
#include <cstdint>

// IPv4 ESP tunnel plaintext parser. Caller must authenticate before using its output.
struct EspLayout {
    size_t esp = 0, plaintext = 0, inner_len = 0, pad = 0;
    uint32_t spi = 0, seq = 0;
};
inline uint16_t layout_be16(const uint8_t *p) {
    return (uint16_t(p[0]) << 8) | p[1];
}
inline uint32_t layout_be32(const uint8_t *p) {
    return (uint32_t(layout_be16(p)) << 16) | layout_be16(p + 2);
}
inline bool parse_esp_layout(const uint8_t *p, size_t n, EspLayout &v) {
    if (n < 14) return false;
    size_t l2 = 14;
    uint16_t type = layout_be16(p + 12);
    while (type == 0x8100 || type == 0x88a8) {
        if (n < l2 + 4) return false;
        type = layout_be16(p + l2 + 2);
        l2 += 4;
    }
    if (type != 0x0800 || n < l2 + 20 || (p[l2] >> 4) != 4) return false;
    size_t ihl = (p[l2] & 15) * 4;
    size_t iplen = layout_be16(p + l2 + 2);
    if (ihl < 20 || iplen < ihl + 16 + 20 + 2 + 16 ||
        iplen > n - l2 || p[l2 + 9] != 50 ||
        (layout_be16(p + l2 + 6) & 0x3fff)) return false;
    v.esp = l2 + ihl;
    v.spi = layout_be32(p + v.esp);
    v.seq = layout_be32(p + v.esp + 4);
    v.plaintext = v.esp + 16; // SPI, sequence, explicit IV.
    size_t trailer = l2 + iplen - 16 - 2; // Retained ICV.
    v.pad = p[trailer];
    if (p[trailer + 1] != 4 || v.pad > trailer - v.plaintext) return false;
    v.inner_len = trailer - v.plaintext - v.pad;
    if (v.inner_len < 20 || (p[v.plaintext] >> 4) != 4) return false;
    size_t inner_ihl = (p[v.plaintext] & 15) * 4;
    if (inner_ihl < 20 || inner_ihl > v.inner_len ||
        layout_be16(p + v.plaintext + 2) != v.inner_len) return false;
    for (size_t j = 0; j < v.pad; ++j)
        if (p[trailer - v.pad + j] != j + 1) return false;
    return true;
}
#endif
