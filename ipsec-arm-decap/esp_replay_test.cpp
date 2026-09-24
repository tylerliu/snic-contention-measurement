#include "esp_replay.h"
#include <cassert>
#include <cstdint>
int main() {
    EspReplay r;
    assert(!r.accept(0));
    assert(r.accept(1));
    assert(!r.accept(1));
    assert(r.accept(256));
    assert(r.accept(2)); // Out-of-order but within the 256-packet window.
    assert(!r.accept(2));
    assert(r.accept(257));
    assert(!r.accept(1)); // Too old.
    assert(r.accept(10000)); // Large jump resets bitmap.
    assert(r.accept(9745));
    assert(!r.accept(9744));
    assert(!r.accept(10000));
    assert(r.accept(UINT32_MAX));
    assert(!r.accept(1)); // No sequence wrap without ESN.
    assert(!r.accept(0));
    EspReplay independent;
    assert(independent.accept(1));
}
