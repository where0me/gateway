#include <assert.h>
#include <stdio.h>
#include "vibration_wire.h"

static uint32_t reference(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return crc;
}

int main(void)
{
    uint8_t data[4097];
    uint32_t random = 12345;
    for (size_t i = 0; i < sizeof(data); i++) {
        random = random * 1664525U + 1013904223U;
        data[i] = (uint8_t)(random >> 24);
    }
    assert(v2_crc((const uint8_t *)"123456789", 9) == 0xcbf43926U);
    assert(v2_crc(NULL, 0) == 0);
    for (size_t n = 0; n <= sizeof(data); n++) {
        uint32_t seed = (uint32_t)n * 2654435761U;
        assert(v2_crc_update(seed, data, n) == reference(seed, data, n));
        size_t split = n / 2;
        uint32_t first = v2_crc_update(seed, data, split);
        assert(v2_crc_update(first, data + split, n - split) ==
               reference(seed, data, n));
    }
    puts("node CRC: known vector, lengths 0..4097, arbitrary seeds and split updates passed");
    return 0;
}
