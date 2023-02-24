#include <cstdint>
#include <nmmintrin.h>

namespace pragzip
{
uint32_t crc32_sse4(uint32_t crc, const void* vec_data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)vec_data;
    const uint32_t* words = (const uint32_t*)bytes;
    size_t len = length / sizeof(uint32_t);

    crc ^= 0xFFFFFFFF;

    while (len >= 4) {
        __m128i data = _mm_loadu_si128((const __m128i*)words);
        crc = _mm_crc32_u32(crc, _mm_cvtsi128_si32(data));
        crc = _mm_crc32_u32(crc, _mm_extract_epi32(data, 1));
        crc = _mm_crc32_u32(crc, _mm_extract_epi32(data, 2));
        crc = _mm_crc32_u32(crc, _mm_extract_epi32(data, 3));
        words += 4;
        len -= 4;
    }

    bytes = (const uint8_t*)words;
    len = length & 3;

    while (len--) {
        crc = _mm_crc32_u8(crc, *bytes++);
    }

    return crc ^ 0xFFFFFFFF;
}
}
