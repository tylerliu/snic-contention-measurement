#include "esp_layout.h"
#include <cassert>
#include <vector>

int main() {
    // Ethernet + IPv4 + ESP/IV + inner IPv4 + two padding bytes + trailer + ICV.
    std::vector<uint8_t> p(90, 0);
    p[12] = 8; p[14] = 0x45; p[17] = 76; p[23] = 50;
    p[36] = 0x20; p[37] = 1; p[41] = 1;
    p[50] = 0x45; p[53] = 20;
    p[70] = 1; p[71] = 2; p[72] = 2; p[73] = 4;
    EspLayout v;
    assert(parse_esp_layout(p.data(), p.size(), v));
    assert(v.spi == 0x2001 && v.seq == 1 && v.plaintext == 50 &&
           v.inner_len == 20 && v.pad == 2);
    for (size_t n = 0; n < p.size(); ++n)
        assert(!parse_esp_layout(p.data(), n, v));
    p[71] = 3;
    assert(!parse_esp_layout(p.data(), p.size(), v));
    p[71] = 2; p[72] = 255;
    assert(!parse_esp_layout(p.data(), p.size(), v));
    p[72] = 2; p[73] = 41;
    assert(!parse_esp_layout(p.data(), p.size(), v));
    p[73] = 4; p[53] = 21;
    assert(!parse_esp_layout(p.data(), p.size(), v));
    p[53] = 20; p[20] = 0x20;
    assert(!parse_esp_layout(p.data(), p.size(), v));
    p[20] = 0;
    p.insert(p.begin() + 12, {0x81, 0, 0, 7});
    assert(parse_esp_layout(p.data(), p.size(), v));
    assert(v.esp == 38 && v.plaintext == 54);
    return 0;
}
