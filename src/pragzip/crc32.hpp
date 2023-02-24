#pragma once

#include <array>
#include <cstdint>
#include <nmmintrin.h>


namespace pragzip
{
/* CRC32 according to RFC 1952 */

using CRC32LookupTable = std::array<uint32_t, 256>;

[[nodiscard]] constexpr CRC32LookupTable
createCRC32LookupTable() noexcept
{
    CRC32LookupTable table{};
    for ( uint32_t n = 0; n < table.size(); ++n ) {
        auto c = static_cast<unsigned long>( n );
        for ( int j = 0; j < 8; ++j ) {
            if ( c & 1UL ) {
                c = 0xEDB88320UL ^ ( c >> 1U );
            } else {
                c >>= 1;
            }
        }
        table[n] = c;
    }
    return table;
}

static constexpr int CRC32_LOOKUP_TABLE_SIZE = 256;

/* a small lookup table: raw data -> CRC32 value to speed up CRC calculation */
alignas( 8 ) constexpr static CRC32LookupTable CRC32_TABLE = createCRC32LookupTable();


[[nodiscard]] constexpr uint32_t
updateCRC32( uint32_t crc,
             uint8_t  data ) noexcept
{
    const auto result = ( crc >> 8U ) ^ CRC32_TABLE[( crc ^ data ) & 0xFFU];
    return result;
}


uint32_t crc32_sse4(uint32_t crc, const void* _data, size_t length);



template <typename T>
inline uint32_t updateCRC32( uint32_t crc, T vec ) noexcept
{
    return crc32_sse4( crc, vec.data(), vec.size() );
}


}
