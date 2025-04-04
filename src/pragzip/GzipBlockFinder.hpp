#pragma once

#include <algorithm>
#include <cassert>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "blockfinder/Bgzf.hpp"
#include "common.hpp"
#include "deflate.hpp"


namespace pragzip
{
/**
 * This is a much more lean version of core/BlockFinder. It does not do any actual work aside from finding the first
 * deflate block. Instead, it is mostly doing bookkeeping and simple partitioning using @ref m_spacingInBits to generate
 * guesses beyond the known block offsets and inside the file range.
 *
 * Block offsets can be confirmed, in which case those will be returned. This is important for performant
 * prefetching and is hard to let the BlockMap do.
 * However, care has to be taken in its usage because block confirmation effectively invalidates block
 * previous indexes!
 */
class GzipBlockFinder
{
public:
    using BlockOffsets = std::vector<size_t>;

public:
    explicit
    GzipBlockFinder( std::unique_ptr<FileReader> fileReader,
                     size_t                      spacing ) :
        m_fileSizeInBits( fileReader->size() * CHAR_BIT ),
        m_spacingInBits( spacing * CHAR_BIT ),
        m_isBgzfFile( blockfinder::Bgzf::isBgzfFile( fileReader ) ),
        m_bgzfBlockFinder( m_isBgzfFile
                           ? std::make_unique<blockfinder::Bgzf>( std::unique_ptr<FileReader>( fileReader->clone() ) )
                           : std::unique_ptr<blockfinder::Bgzf>() )
    {
        if ( m_spacingInBits < 32_Ki ) {
            /* Well, actually, it could make sense because this is about the spacing in the compressed data but
             * then even more! A spacing of 32 KiB in uncompressed data can lead to index sizes up to the
             * decompressed file. A spacing of 32 KiB in the compressed data can only lead to an index equal that
             * of the compressed file, so it behaves much more reasonable! */
            throw std::invalid_argument( "A spacing smaller than the window size makes no sense!" );
        }

        /**
         * @todo I'm not sure whether it should skip empty files and/or detect pigz files? Maybe both for stability.
         *       Currently it fails for pigz!
         */

        /* The first deflate block offset is easily found by reading other the gzip header.
         * The correctness and existence of this first block is a required initial condition for the algorithm. */
        BitReader bitReader{ std::move( fileReader ) };
        const auto [header, error] = gzip::readHeader( bitReader );
        if ( error != Error::NONE ) {
            throw std::invalid_argument( "Encountered error while reading gzip header: " + toString( error ) );
        }
        m_blockOffsets.push_back( bitReader.tell() );
    }

    /**
     * @return number of block offsets. This number may increase as long as it is not finalized yet.
     */
    [[nodiscard]] size_t
    size() const
    {
        std::scoped_lock lock( m_mutex );
        return m_blockOffsets.size();
    }

    void
    finalize()
    {
        std::scoped_lock lock( m_mutex );
        m_finalized = true;
    }

    [[nodiscard]] bool
    finalized() const
    {
        std::scoped_lock lock( m_mutex );
        return m_finalized;
    }

    [[nodiscard]] bool
    isBgzfFile() const noexcept
    {
        return m_isBgzfFile;
    }

    /**
     * Insert a known to be exact block offset. They should in general be inserted in sequence because no
     * partitioning will be done before the largest inserted block offset.
     */
    void
    insert( size_t blockOffset )
    {
        std::scoped_lock lock( m_mutex );
        insertUnsafe( blockOffset );
    }

    /**
     * @return The block offset to the given block index or nothing when the block finder is finalized and the
     *         requested block out of range. When the requested block index is not a known one, a guess will
     *         be returned based on @ref m_spacingInBits.
     * @todo ADD TESTS FOR THIS
     */
    [[nodiscard]] std::optional<size_t>
    get( size_t                  blockIndex,
         [[maybe_unused]] double timeoutInSeconds = std::numeric_limits<double>::infinity() )
    {
        std::scoped_lock lock( m_mutex );

        if ( m_isBgzfFile && m_bgzfBlockFinder && !m_finalized ) {
            gatherMoreBgzfBlocks( blockIndex );
        }

        if ( blockIndex < m_blockOffsets.size() ) {
            return m_blockOffsets[blockIndex];
        };

        assert( !m_blockOffsets.empty() );
        const auto blockIndexOutside = blockIndex - m_blockOffsets.size();  // >= 0
        const auto partitionIndex = firstPartitionIndex() + blockIndexOutside;
        const auto blockOffset = partitionIndex * m_spacingInBits;
        if ( blockOffset < m_fileSizeInBits ) {
            return blockOffset;
        }

        /* As the last offset (one after the last valid one), return the file size. */
        if ( partitionIndex > 0 ) {
            const auto previousBlockOffset = ( partitionIndex - 1U ) * m_spacingInBits;
            if ( previousBlockOffset < m_fileSizeInBits ) {
                return m_fileSizeInBits;
            }
        }

        return std::nullopt;
    }

    /**
     * @return Index for the block at the requested offset.
     */
    [[nodiscard]] size_t
    find( size_t encodedBlockOffsetInBits ) const
    {
        std::scoped_lock lock( m_mutex );

        /* Find in sorted vector by bisection. */
        const auto match = std::lower_bound( m_blockOffsets.begin(), m_blockOffsets.end(), encodedBlockOffsetInBits );
        if ( ( match != m_blockOffsets.end() ) && ( *match == encodedBlockOffsetInBits ) ) {
            return std::distance( m_blockOffsets.begin(), match );
        }

        if ( ( encodedBlockOffsetInBits > m_blockOffsets.back() )
             && ( encodedBlockOffsetInBits < m_fileSizeInBits )
             && ( encodedBlockOffsetInBits % m_spacingInBits == 0 ) )
        {
            const auto blockIndex = m_blockOffsets.size()
                                    + ( encodedBlockOffsetInBits / m_spacingInBits - firstPartitionIndex() );
            assert( ( firstPartitionIndex() + ( blockIndex - m_blockOffsets.size() ) ) * m_spacingInBits
                    == encodedBlockOffsetInBits /* see get for the inverse calculation this is taken from. */ );
            return blockIndex;
        }

        throw std::out_of_range( "No block with the specified offset " + std::to_string( encodedBlockOffsetInBits )
                                 + " exists in the block finder map!" );
    }

    void
    setBlockOffsets( const std::vector<size_t>& blockOffsets )
    {
        m_blockOffsets.assign( blockOffsets.begin(), blockOffsets.end() );
        finalize();
    }

    [[nodiscard]] size_t
    partitionOffsetContainingOffset( size_t blockOffset ) const
    {
        /* Round down to m_spacingInBits grid. */
        return ( blockOffset / m_spacingInBits ) * m_spacingInBits;
    }

    [[nodiscard]] constexpr size_t
    spacingInBits() const noexcept
    {
        return m_spacingInBits;
    }

private:
    void
    insertUnsafe( size_t blockOffset )
    {
        if ( blockOffset >= m_fileSizeInBits ) {
            return;
        }

        const auto match = std::lower_bound( m_blockOffsets.begin(), m_blockOffsets.end(), blockOffset );
        if ( ( match == m_blockOffsets.end() ) || ( *match != blockOffset ) ) {
            if ( m_finalized ) {
                throw std::invalid_argument( "Already finalized, may not insert further block offsets!" );
            }
            m_blockOffsets.insert( match, blockOffset );
            assert( std::is_sorted( m_blockOffsets.begin(), m_blockOffsets.end() ) );
        }
    }

    void
    gatherMoreBgzfBlocks( size_t blockNumber )
    {
        while ( blockNumber + m_batchFetchCount >= m_blockOffsets.size() ) {
            const auto nextOffset = m_bgzfBlockFinder->find();
            if ( nextOffset < m_blockOffsets.back() + m_spacingInBits ) {
                continue;
            }
            if ( nextOffset >= m_fileSizeInBits ) {
                break;
            }
            insertUnsafe( nextOffset );
        }
    }

    /**
     * @return the "index" corresponding to the first "guessed" block offset given by the formula i * m_spacingInBits
     *         for i in N_0 with the requirement that it must be larger (not equal) than the last confirmed offset.
     */
    [[nodiscard]] size_t
    firstPartitionIndex() const
    {
        /* Consider a spacing of 2. The guesses would return offsets at 0, 2, 4, 6, ...
         * If the last confirmed offset was 0 or 1 , then the next partition offset would be 2, i.e.,
         * we should return the index 1. If the last confirmed offset was 2 or 3, we should return 2 and so on.
         * This means we want to divide by the spacing and round the result down and add plus 1 to that. */
        return m_blockOffsets.back() / m_spacingInBits + 1;
    }

private:
    mutable std::mutex m_mutex;

    size_t const m_fileSizeInBits;
    bool m_finalized{ false };
    size_t const m_spacingInBits;

    /**
     * These should only contain confirmed block offsets in order. Use a deque to avoid having to move all
     * subsequent elements when inserting into the sorted container.
     */
    std::deque<size_t> m_blockOffsets;

    /** Only used for Bgzf files in which case it will gather offsets in chunks of this. */
    const bool m_isBgzfFile;
    const std::unique_ptr<blockfinder::Bgzf> m_bgzfBlockFinder;
    const size_t m_batchFetchCount = std::max<size_t>( 16, 3U * std::thread::hardware_concurrency() );
};
}  // namespace pragzip
