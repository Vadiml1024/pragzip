#pragma once

#include <array>
#include <cstdint>
#include <cstring>


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
    return ( crc >> 8U ) ^ CRC32_TABLE[( crc ^ data ) & 0xFFU];
}


static constexpr size_t MAX_CRC32_SLICE_SIZE = 64;

/**
 * @see https://ieeexplore.ieee.org/document/4531728
 * @see https://create.stephan-brumme.com/crc32/#slicing-by-16-overview
 * @note LUT[n + 1] contains the CRC32 of a byte steam consisting of n zero-bytes.
 */
alignas( 8 ) static constexpr std::array<std::array<uint32_t, 256>, MAX_CRC32_SLICE_SIZE> CRC32_SLICE_BY_N_LUT =
    [] ()
    {
        std::array<std::array<uint32_t, 256>, MAX_CRC32_SLICE_SIZE> lut{};
        lut[0] = CRC32_TABLE;
        for ( size_t i = 0; i < lut[0].size(); ++i ) {
            for ( size_t j = 1; j < lut.size(); ++j ) {
                lut[j][i] = updateCRC32( lut[j - 1][i], 0 );
            }
        }
        return lut;
    }();


template<unsigned int SLICE_SIZE>
[[nodiscard]] uint32_t
crc32SliceByN( uint32_t    crc,
               const char* data,
               size_t      size )
{
    static_assert( SLICE_SIZE % 4 == 0, "Chunk size must be divisible by 4 because of the loop unrolling." );
    static_assert( SLICE_SIZE > 0, "Chunk size must not be 0." );
    static_assert( SLICE_SIZE <= MAX_CRC32_SLICE_SIZE, "Chunk size must not exceed the lookup table size." );

    crc = ~crc;

    constexpr auto& LUT = CRC32_SLICE_BY_N_LUT;
    /* Unrolling by 8 increases speed from 4 GB/s to 4.5 GB/s (+12.5%).
     * Might be CPU-dependent (instruction cache size, ...). */
    #pragma GCC unroll 8
    for ( size_t i = 0; i + SLICE_SIZE <= size; i += SLICE_SIZE ) {
        uint32_t firstDoubleWord;
        std::memcpy( &firstDoubleWord, data + i, sizeof( uint32_t ) );
        crc ^= firstDoubleWord;

        alignas( 8 ) std::array<uint8_t, SLICE_SIZE> chunk;
        std::memcpy( chunk.data(), &crc, sizeof( uint32_t ) );
        std::memcpy( chunk.data() + sizeof( uint32_t ),
                     data + i + sizeof( uint32_t ),
                     SLICE_SIZE - sizeof( uint32_t ) );

        uint32_t result = 0;
        /* Has no effect. I assume it is automatically unrolled with -O3 even without this. */
        #pragma GCC unroll 16
        for ( size_t j = 0; j < SLICE_SIZE; ++j ) {
            result ^= LUT[j][chunk[SLICE_SIZE - 1 - j]];
        }
        crc = result;
    }

    for ( size_t i = size - ( size % SLICE_SIZE ); i < size; ++i ) {
        crc = updateCRC32( crc, data[i] );
    }

    return ~crc;
}


template<unsigned int SLICE_SIZE>
[[nodiscard]] uint32_t
updateCRC32( const uint32_t    crc,
             const char* const buffer,
             const size_t      size )
{
    return crc32SliceByN<SLICE_SIZE>( crc, buffer, size );
}
}  // namespace pragzip
