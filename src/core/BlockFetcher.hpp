#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <AffinityHelpers.hpp>
#include <Cache.hpp>
#include <common.hpp>
#include <Prefetcher.hpp>
#include <ThreadPool.hpp>


/**
 * Manages block data access. Calls to members are not thread-safe!
 * Requested blocks are cached and accesses may trigger prefetches,
 * which will be fetched in parallel using a thread pool.
 */
template<typename T_BlockFinder,
         typename T_BlockData,
         typename FetchingStrategy,
         bool     ENABLE_STATISTICS = false,
         bool     SHOW_PROFILE = false>
class BlockFetcher
{
public:
    using BlockFinder = T_BlockFinder;
    using BlockData = T_BlockData;
    using BlockCache = Cache</** block offset in bits */ size_t, std::shared_ptr<BlockData> >;

    using GetPartitionOffset = std::function<size_t( size_t )>;

    struct Statistics
    {
    public:
        [[nodiscard]] double
        cacheHitRate() const
        {
            return static_cast<double>( cache.hits + prefetchCache.hits + prefetchDirectHits )
                   / static_cast<double>( gets );
        }

        [[nodiscard]] double
        uselessPrefetches() const
        {
            const auto totalFetches = prefetchCount + onDemandFetchCount;
            if ( totalFetches == 0 ) {
                return 0;
            }
            return static_cast<double>( prefetchCache.unusedEntries ) / static_cast<double>( totalFetches );
        }

        [[nodiscard]] std::string
        print() const
        {
            std::stringstream existingBlocks;
            existingBlocks << ( blockCountFinalized ? "" : ">=" ) << blockCount;

            const auto decodeDuration = decodeBlockStartTime && decodeBlockEndTime
                                        ? duration( *decodeBlockStartTime, *decodeBlockEndTime )
                                        : 0.0;
            const auto optimalDecodeDuration = decodeBlockTotalTime / parallelization;
            /* The pool efficiency only makes sense when the thread pool is smaller or equal the CPU cores. */
            const auto poolEfficiency = optimalDecodeDuration / decodeDuration;

            std::stringstream out;
            out << "\n   Parallelization                   : " << parallelization
                << "\n   Cache"
                << "\n       Hits                          : " << cache.hits
                << "\n       Misses                        : " << cache.misses
                << "\n       Unused Entries                : " << cache.unusedEntries
                << "\n       Maximum Fill Size             : " << cache.maxSize
                << "\n       Capacity                      : " << cache.capacity
                << "\n   Prefetch Cache"
                << "\n       Hits                          : " << prefetchCache.hits
                << "\n       Misses                        : " << prefetchCache.misses
                << "\n       Unused Entries                : " << prefetchCache.unusedEntries
                << "\n       Prefetch Queue Hit            : " << prefetchDirectHits
                << "\n       Maximum Fill Size             : " << prefetchCache.maxSize
                << "\n       Capacity                      : " << prefetchCache.capacity
                << "\n   Cache Hit Rate                    : " << cacheHitRate() * 100 << " %"
                << "\n   Useless Prefetches                : " << uselessPrefetches() * 100 << " %"
                << "\n   Access Patterns"
                << "\n       Total Accesses                : " << gets
                << "\n       Duplicate Block Accesses      : " << repeatedBlockAccesses
                << "\n       Sequential Block Accesses     : " << sequentialBlockAccesses
                << "\n       Block Seeks Back              : " << backwardBlockAccesses
                << "\n       Block Seeks Forward           : " << forwardBlockAccesses
                << "\n   Blocks"
                << "\n       Total Existing                : " << existingBlocks.str()
                << "\n       Total Fetched                 : " << prefetchCount + onDemandFetchCount
                << "\n       Prefetched                    : " << prefetchCount
                << "\n       Fetched On-demand             : " << onDemandFetchCount
                << "\n   Prefetch Stall by BlockFinder     : " << waitOnBlockFinderCount
                << "\n   Time spent in:"
                << "\n       bzip2::readBlockData          : " << readBlockDataTotalTime << " s"
                << "\n       decodeBlock                   : " << decodeBlockTotalTime   << " s"
                << "\n       std::future::get              : " << futureWaitTotalTime    << " s"
                << "\n       get                           : " << getTotalTime           << " s"
                << "\n   Thread Pool Utilization:"
                << "\n       Total Real Decode Duration    : " << decodeDuration << " s"
                << "\n       Theoretical Optimal Duration  : " << optimalDecodeDuration << " s"
                << "\n       Pool Efficiency (Fill Factor) : " << poolEfficiency * 100 << " %";
            return out.str();
        }

        void
        recordBlockIndexGet( size_t blockindex )
        {
            ++gets;

            if ( !lastAccessedBlock ) {
                lastAccessedBlock = blockindex;  // effectively counts the first access as sequential
            }

            if ( blockindex > *lastAccessedBlock + 1 ) {
                forwardBlockAccesses++;
            } else if ( blockindex < *lastAccessedBlock ) {
                backwardBlockAccesses++;
            } else if ( blockindex == *lastAccessedBlock ) {
                repeatedBlockAccesses++;
            } else {
                sequentialBlockAccesses++;
            }

            lastAccessedBlock = blockindex;
        }

    public:
        size_t parallelization{ 0 };
        size_t blockCount{ 0 };
        bool blockCountFinalized{ false };

        typename BlockCache::Statistics cache;
        typename BlockCache::Statistics prefetchCache;

        size_t gets{ 0 };
        std::optional<size_t> lastAccessedBlock;
        size_t repeatedBlockAccesses{ 0 };
        size_t sequentialBlockAccesses{ 0 };
        size_t backwardBlockAccesses{ 0 };
        size_t forwardBlockAccesses{ 0 };

        size_t onDemandFetchCount{ 0 };
        size_t prefetchCount{ 0 };
        size_t prefetchDirectHits{ 0 };
        size_t waitOnBlockFinderCount{ 0 };

        std::optional<std::decay_t<decltype( now() )> > decodeBlockStartTime;
        std::optional<std::decay_t<decltype( now() )> > decodeBlockEndTime;

        double decodeBlockTotalTime{ 0 };
        double futureWaitTotalTime{ 0 };
        double getTotalTime{ 0 };
        double readBlockDataTotalTime{ 0 };
    };

protected:
    BlockFetcher( std::shared_ptr<BlockFinder> blockFinder,
                  size_t                       parallelization ) :
        m_parallelization( parallelization == 0
                           ? std::max<size_t>( 1U, availableCores() )
                           : parallelization ),
        m_blockFinder    ( std::move( blockFinder ) ),
        m_cache          ( std::max( size_t( 16 ), m_parallelization ) ),
        m_prefetchCache  ( 2 * m_parallelization /* Only m_parallelization would lead to lot of cache pollution! */ ),
        m_threadPool     ( m_parallelization )
    {
        if ( !m_blockFinder ) {
            throw std::invalid_argument( "BlockFinder must be valid!" );
        }

        if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
            m_statistics.parallelization = m_parallelization;
        }
    }

public:
    virtual
    ~BlockFetcher()
    {
        if constexpr ( SHOW_PROFILE ) {
            /* Clear caches while updating the unused entries statistic. */
            m_cache.shrinkTo( 0 );
            m_prefetchCache.shrinkTo( 0 );
            std::cerr << ( ThreadSafeOutput() << "[BlockFetcher::~BlockFetcher]" << statistics().print() );
        }
    }

    /**
     * Fetches, prefetches, caches, and returns result.
     * @param dataBlockIndex Only used to determine which block indexes to prefetch. If not specified, will
     *        query BlockFinder for the block offset. This started as a performance optimization, to avoid
     *        unnecessary BlockFinder lookups but when looking up the partition offset, it might be necessary
     *        or else the BlockFinder::find call might throw because it can't find the given offset.
     * @param getPartitionOffsetFromOffset Returns the partition offset to a given blockOffset. This is used to look up
     *        existence of blocks in the cache to avoid duplicate prefetches (one for the partition offset
     *        and another one for the real offset).
     * @return BlockData to requested blockOffset. Undefined what happens for an invalid blockOffset as input.
     */
    [[nodiscard]] std::shared_ptr<BlockData>
    get( const size_t                blockOffset,
         const std::optional<size_t> dataBlockIndex = std::nullopt,
         const bool                  onlyCheckCaches = false,
         const GetPartitionOffset&   getPartitionOffsetFromOffset = {} )
    {
        [[maybe_unused]] const auto tGetStart = now();

        /* Not using capture bindings here because C++ is too dumb to capture those, yet.
         * @see https://stackoverflow.com/a/46115028/2191065 */
        auto resultFromCaches = getFromCaches( blockOffset );
        auto& cachedResult = resultFromCaches.first;
        auto& queuedResult = resultFromCaches.second;

        const auto validDataBlockIndex = dataBlockIndex ? *dataBlockIndex : m_blockFinder->find( blockOffset );
        const auto nextBlockOffset = m_blockFinder->get( validDataBlockIndex + 1 );

        if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
            m_statistics.recordBlockIndexGet( validDataBlockIndex );
        }

        /* Start requested calculation if necessary. */
        if ( !cachedResult.has_value() && !queuedResult.valid() ) {
            if ( onlyCheckCaches ) {
                return {};
            }
            queuedResult = submitOnDemandTask( blockOffset, nextBlockOffset );
        }

        m_fetchingStrategy.fetch( validDataBlockIndex );

        const auto resultIsReady =
            [&cachedResult, &queuedResult] () {
                using namespace std::chrono_literals;
                return cachedResult.has_value() ||
                       ( queuedResult.valid() && ( queuedResult.wait_for( 0s ) == std::future_status::ready ) );
            };

        prefetchNewBlocks( getPartitionOffsetFromOffset, resultIsReady );

        /* Return result */
        if ( cachedResult.has_value() ) {
            assert( !queuedResult.valid() );
            if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
                std::scoped_lock lock( m_analyticsMutex );
                m_statistics.getTotalTime += duration( tGetStart );
            }
            return *std::move( cachedResult );
        }

        [[maybe_unused]] const auto tFutureGetStart = now();
        using namespace std::chrono_literals;
        /* At ~4 MiB compressed blocks and ~200 MB/s compressed bandwidth for base64, one block might take ~20ms. */
        while ( queuedResult.wait_for( 1ms ) == std::future_status::timeout ) {
            prefetchNewBlocks( getPartitionOffsetFromOffset, resultIsReady );
        }
        auto result = std::make_shared<BlockData>( queuedResult.get() );
        [[maybe_unused]] const auto futureGetDuration = duration( tFutureGetStart );

        insertIntoCache( blockOffset, result );

        if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
            std::scoped_lock lock( m_analyticsMutex );
            m_statistics.futureWaitTotalTime += futureGetDuration;
            m_statistics.getTotalTime += duration( tGetStart );
        }

        return result;
    }

    void
    clearCache()
    {
        m_cache.clear();
    }

    [[nodiscard]] Statistics
    statistics() const
    {
        auto result = m_statistics;
        if ( m_blockFinder ) {
            result.blockCountFinalized = m_blockFinder->finalized();
            result.blockCount = m_blockFinder->size();
        }
        result.cache = m_cache.statistics();
        result.prefetchCache = m_prefetchCache.statistics();
        result.readBlockDataTotalTime = m_readBlockDataTotalTime;
        return result;
    }

private:
    void
    insertIntoCache( const size_t               blockOffset,
                     std::shared_ptr<BlockData> blockData )
    {
        if ( m_fetchingStrategy.isSequential() ) {
            m_cache.clear();
        }
        m_cache.insert( blockOffset, std::move( blockData ) );
    }


    [[nodiscard]] bool
    isInCacheOrQueue( const size_t blockOffset ) const
    {
        return ( m_prefetching.find( blockOffset ) != m_prefetching.end() )
               || m_cache.test( blockOffset )
               || m_prefetchCache.test( blockOffset );
    }

    /**
     * @return either a shared_ptr from the caches or a future from the prefetch queue. The prefetch future
     *         is taken from the queue, i.e., it should not be discarded. Either reinsert it into the queue
     *         or wait for the result and insert it into a cache.
     */
    [[nodiscard]] std::pair<std::optional<std::shared_ptr<BlockData> >, std::future<BlockData> >
    getFromCaches( const size_t blockOffset )
    {
        /* In case of a late prefetch, this might return an unfinished future. */
        auto resultFuture = takeFromPrefetchQueue( blockOffset );

        /* Access cache before data might get evicted!
         * Access cache after prefetch queue to avoid incrementing the cache misses counter. */
        std::optional<std::shared_ptr<BlockData> > result;
        if ( !resultFuture.valid() ) {
            result = m_cache.get( blockOffset );
            if ( !result ) {
                /* On prefetch cache hit, move the value into the normal cache. */
                result = m_prefetchCache.get( blockOffset );
                if ( result ) {
                    m_prefetchCache.evict( blockOffset );
                    insertIntoCache( blockOffset, *result );
                }
            }
        }

        return std::make_pair( std::move( result ), std::move( resultFuture ) );
    }

    [[nodiscard]] std::future<BlockData>
    takeFromPrefetchQueue( size_t blockOffset )
    {
        /* Check whether the desired offset is prefetched. */
        std::future<BlockData> resultFuture;
        const auto match = m_prefetching.find( blockOffset );

        if ( match != m_prefetching.end() ) {
            resultFuture = std::move( match->second );
            m_prefetching.erase( match );
            assert( resultFuture.valid() );

            if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
                ++m_statistics.prefetchDirectHits;
            }
        }

        return resultFuture;
    }

    /* Check for ready prefetches and move them to cache. */
    void
    processReadyPrefetches()
    {
        using namespace std::chrono_literals;

        for ( auto it = m_prefetching.begin(); it != m_prefetching.end(); ) {
            auto& [prefetchedBlockOffset, prefetchedFuture] = *it;

            if ( prefetchedFuture.valid() && ( prefetchedFuture.wait_for( 0s ) == std::future_status::ready ) ) {
                BlockData result;
                try {
                    result = prefetchedFuture.get();
                    m_prefetchCache.insert( prefetchedBlockOffset, std::make_shared<BlockData>( std::move( result ) ) );
                } catch ( ... ) {
                    /* Prefetching failed, ignore result and error. If the error was a real one, then it will
                     * will be rethrown when the task is requested directly and run directly. */
                }
                it = m_prefetching.erase( it );
            } else {
                ++it;
            }
        }
    }

    /**
     * Fills m_prefetching up with a maximum of m_parallelization-1 new tasks predicted
     * based on the given last accessed block index(es).
     * @param stopPrefetching The prefetcher might wait a bit on the block finder but when stopPrefetching returns true
     *                        it will stop that and return before having completely filled the prefetch queue.
     */
    void
    prefetchNewBlocks( const GetPartitionOffset&    getPartitionOffsetFromOffset,
                       const std::function<bool()>& stopPrefetching )
    {
        /* Make space for new asynchronous prefetches. */
        processReadyPrefetches();

        const auto threadPoolSaturated =
            [&] () { return m_prefetching.size() + /* thread with the requested block */ 1 >= m_threadPool.size(); };

        if ( threadPoolSaturated() ) {
            return;
        }

        auto blockIndexesToPrefetch = m_fetchingStrategy.prefetch( m_prefetchCache.capacity() );

        std::vector<size_t> blockOffsetsToPrefetch( blockIndexesToPrefetch.size() );
        for ( auto blockIndexToPrefetch : blockIndexesToPrefetch ) {
            /* If we don't find the offset in the timeout of 0, then we very likely also don't have it cached yet. */
            const auto blockOffset = m_blockFinder->get( blockIndexToPrefetch, /* timeout */ 0 );
            if ( !blockOffset ) {
                continue;
            }

            blockOffsetsToPrefetch.emplace_back( *blockOffset );
            if ( getPartitionOffsetFromOffset ) {
                const auto partitionOffset = getPartitionOffsetFromOffset( *blockOffset );
                if ( *blockOffset != partitionOffset ) {
                    blockOffsetsToPrefetch.emplace_back( partitionOffset );
                }
            }
        }

        const auto touchInCacheIfExists =
            [this] ( size_t prefetchBlockOffset )
            {
                if ( m_prefetchCache.test( prefetchBlockOffset ) ) {
                    m_prefetchCache.touch( prefetchBlockOffset );
                }
                if ( m_cache.test( prefetchBlockOffset ) ) {
                    m_cache.touch( prefetchBlockOffset );
                }
            };

        /* Touch all blocks to be prefetched to avoid evicting them while doing the prefetching of other blocks! */
        for ( auto offset = blockOffsetsToPrefetch.rbegin(); offset != blockOffsetsToPrefetch.rend(); ++offset ) {
            touchInCacheIfExists( *offset );
        }

        for ( auto blockIndexToPrefetch : blockIndexesToPrefetch ) {
            if ( threadPoolSaturated() ) {
                break;
            }

            if ( m_blockFinder->finalized() && ( blockIndexToPrefetch >= m_blockFinder->size() ) ) {
                continue;
            }

            /* If the block with the requested index has not been found yet and if we have to wait on the requested
             * result future anyway, then wait a non-zero amount of time on the BlockFinder! */
            std::optional<size_t> prefetchBlockOffset;
            std::optional<size_t> nextPrefetchBlockOffset;
            do
            {
                prefetchBlockOffset = m_blockFinder->get( blockIndexToPrefetch, stopPrefetching() ? 0 : 0.0001 );
                const auto wasFinalized = m_blockFinder->finalized();
                nextPrefetchBlockOffset = m_blockFinder->get( blockIndexToPrefetch + 1,
                                                              stopPrefetching() ? 0 : 0.0001 );
                if ( wasFinalized && !nextPrefetchBlockOffset ) {
                    nextPrefetchBlockOffset = std::numeric_limits<size_t>::max();
                }
            }
            while ( !prefetchBlockOffset && !nextPrefetchBlockOffset && !stopPrefetching() );

            if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
                if ( !prefetchBlockOffset.has_value() ) {
                    m_statistics.waitOnBlockFinderCount++;
                }
            }

            /* Do not prefetch already cached/prefetched blocks or block indexes which are not yet in the block map. */
            if ( !prefetchBlockOffset.has_value()
                 || !nextPrefetchBlockOffset.has_value()
                 || isInCacheOrQueue( *prefetchBlockOffset )
                 || ( getPartitionOffsetFromOffset
                      && isInCacheOrQueue( getPartitionOffsetFromOffset( *prefetchBlockOffset ) ) ) )
            {
                continue;
            }

            /* Avoid cache pollution by stopping prefetching when we would evict usable results.
             * Note that we have to also account for m_prefetching.size() evictions before our eviction of interest! */
            if ( const auto offsetToBeEvicted = m_prefetchCache.nextNthEviction( m_prefetching.size() + 1 );
                 offsetToBeEvicted.has_value() ) {
                if ( contains( blockOffsetsToPrefetch, offsetToBeEvicted ) ) {
                    break;
                }
            }

            ++m_statistics.prefetchCount;
            auto prefetchedFuture = m_threadPool.submit(
                [this, offset = *prefetchBlockOffset, nextOffset = *nextPrefetchBlockOffset] () {
                    return decodeAndMeasureBlock( offset, nextOffset );
                }, /* priority */ 0 );
            const auto [_, wasInserted] = m_prefetching.emplace( *prefetchBlockOffset, std::move( prefetchedFuture ) );
            if ( !wasInserted ) {
                std::logic_error( "Submitted future could not be inserted to prefetch queue!" );
            }
        }

        /* Note that only m_parallelization-1 blocks will be prefetched. Meaning that even with the unconditionally
         * submitted requested block, the thread pool should never contain more than m_parallelization tasks!
         * All tasks submitted to the thread pool, should either exist in m_prefetching or only temporary inside
         * 'resultFuture' in the 'read' method. */
        if ( m_threadPool.unprocessedTasksCount( 0 ) > m_parallelization ) {
            throw std::logic_error( "The thread pool should not have more tasks than there are prefetching futures!" );
        }
    }

    [[nodiscard]] std::future<BlockData>
    submitOnDemandTask( const size_t                blockOffset,
                        const std::optional<size_t> nextBlockOffset )
    {
        if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
            ++m_statistics.onDemandFetchCount;
        }
        auto resultFuture = m_threadPool.submit( [this, blockOffset, nextBlockOffset] () {
            return decodeAndMeasureBlock(
                blockOffset, nextBlockOffset ? *nextBlockOffset : std::numeric_limits<size_t>::max() );
        }, /* priority */ 0 );
        assert( resultFuture.valid() );
        return resultFuture;
    }

protected:
    [[nodiscard]] virtual BlockData
    decodeBlock( size_t blockOffset,
                 size_t nextBlockOffset ) const = 0;

    /**
     * This must be called before variables that are used by @ref decodeBlock are destructed, i.e.,
     * it must be called in the inheriting class.
     */
    void
    stopThreadPool()
    {
        m_threadPool.stop();
    }

    template<class T_Functor>
    std::future<decltype( std::declval<T_Functor>()() )>
    submitTaskWithHighPriority( T_Functor task )
    {
        return m_threadPool.submit( std::move( task ), /* priority */ -1 );
    }


    [[nodiscard]] const auto&
    cache() const noexcept
    {
        return m_cache;
    }

    [[nodiscard]] const auto&
    prefetchCache() const noexcept
    {
        return m_prefetchCache;
    }

private:
    [[nodiscard]] BlockData
    decodeAndMeasureBlock( size_t blockOffset,
                           size_t nextBlockOffset ) const
    {
        [[maybe_unused]] const auto tDecodeStart = now();
        auto blockData = decodeBlock( blockOffset, nextBlockOffset );
        if constexpr ( ENABLE_STATISTICS || SHOW_PROFILE ) {
            const auto tDecodeEnd = now();

            std::scoped_lock lock( this->m_analyticsMutex );

            const auto& minStartTime = this->m_statistics.decodeBlockStartTime;
            this->m_statistics.decodeBlockStartTime.emplace(
                minStartTime ? std::min( *minStartTime, tDecodeStart ) : tDecodeStart );

            const auto& maxEndTime = this->m_statistics.decodeBlockEndTime;
            this->m_statistics.decodeBlockEndTime.emplace(
                maxEndTime ? std::max( *maxEndTime, tDecodeEnd ) : tDecodeEnd );

            this->m_statistics.decodeBlockTotalTime += duration( tDecodeStart, tDecodeEnd );
        }
        return blockData;
    }

private:
    /* Analytics */
    mutable Statistics m_statistics;

protected:
    mutable double m_readBlockDataTotalTime{ 0 };
    mutable std::mutex m_analyticsMutex;

private:
    const size_t m_parallelization;

    /**
     * The block finder is used to prefetch blocks among others.
     * But, in general, it only returns unconfirmed guesses for block offsets (at first)!
     * Confirmed block offsets are written to the BlockMap but adding that in here seems a bit overkill
     * and would need further logic to get the next blocks given a specific one.
     * Therefore, the idea is to update and confirm the blocks inside the block finder, which would invalidate
     * the block indexes! In order for that, to not lead to problems, the block finder should only be used by
     * the managing thread not by the worker threads!
     */
    const std::shared_ptr<BlockFinder> m_blockFinder;

    BlockCache m_cache;
    BlockCache m_prefetchCache;
    FetchingStrategy m_fetchingStrategy;

    std::map</* block offset */ size_t, std::future<BlockData> > m_prefetching;
    ThreadPool m_threadPool;
};
