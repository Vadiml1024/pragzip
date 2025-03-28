#pragma once

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/stat.h>

#ifdef _MSC_VER
    #define NOMINMAX
    #include <Windows.h>
#else
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <errno.h>
    #include <fcntl.h>
    #include <limits.h>         // IOV_MAX
    #include <sys/stat.h>
    #include <sys/poll.h>
    #include <sys/uio.h>
    #include <unistd.h>

    #if not defined( HAVE_VMSPLICE ) and defined( __linux__ )
        #define HAVE_VMSPLICE
    #endif

    #if not defined( HAVE_IOVEC ) and defined( __linux__ )
        #define HAVE_IOVEC
    #endif
#endif


#if defined( HAVE_VMSPLICE )
    #include <any>
    #include <deque>

    #include <AtomicMutex.hpp>
#endif


#ifdef _MSC_VER
[[nodiscard]] bool
stdinHasInput()
{
    const auto handle = GetStdHandle( STD_INPUT_HANDLE );
    DWORD bytesAvailable{ 0 };
    const auto success = PeekNamedPipe( handle, nullptr, 0, nullptr, &bytesAvailable, nullptr );
    return ( success == 0 ) && ( bytesAvailable > 0 );
}


[[nodiscard]] bool
stdoutIsDevNull()
{
    /**
     * @todo Figure this out on Windows in a reasonable readable manner:
     * @see https://stackoverflow.com/a/21070689/2191065
     */
    return false;
}

#else

[[nodiscard]] bool
stdinHasInput()
{
    pollfd fds{};
    fds.fd = STDIN_FILENO;
    fds.events = POLLIN;
    return poll( &fds, 1, /* timeout in ms */ 0 ) == 1;
}


[[nodiscard]] bool
stdoutIsDevNull()
{
    struct stat devNull{};
    struct stat stdOut{};
    return ( fstat( STDOUT_FILENO, &stdOut ) == 0 ) &&
           ( stat( "/dev/null", &devNull ) == 0 ) &&
           S_ISCHR( stdOut.st_mode ) &&  // NOLINT
           ( devNull.st_dev == stdOut.st_dev ) &&
           ( devNull.st_ino == stdOut.st_ino );
}
#endif


inline bool
fileExists( const std::string& filePath )
{
    return std::ifstream( filePath ).good();
}


inline size_t
fileSize( const std::string& filePath )
{
    std::ifstream file( filePath );
    file.seekg( 0, std::ios_base::end );
    const auto result = file.tellg();
    if ( result < 0 ) {
        throw std::invalid_argument( "Could not get size of specified file!" );
    }
    return static_cast<size_t>( result );
}


inline size_t
filePosition( std::FILE* file )
{
    const auto offset = std::ftell( file );
    if ( offset < 0 ) {
        throw std::runtime_error( "Could not get the file position!" );
    }
    return static_cast<size_t>( offset );
}


#ifndef _MSC_VER
struct unique_file_descriptor
{
    explicit
    unique_file_descriptor( int fd ) :
        m_fd( fd )
    {}

    ~unique_file_descriptor()
    {
        close();
    }

    unique_file_descriptor() = default;
    unique_file_descriptor( const unique_file_descriptor& ) = delete;
    unique_file_descriptor& operator=( const unique_file_descriptor& ) = delete;

    unique_file_descriptor( unique_file_descriptor&& other ) noexcept :
        m_fd( other.m_fd )
    {
        other.m_fd = -1;
    }

    unique_file_descriptor&
    operator=( unique_file_descriptor&& other ) noexcept
    {
        close();
        m_fd = other.m_fd;
        other.m_fd = -1;
        return *this;
    }

    [[nodiscard]] constexpr int
    operator*() const noexcept
    {
        return m_fd;
    }

    void
    close()
    {
        if ( m_fd >= 0 ) {
            ::close( m_fd );
        }
    }

    void
    release()
    {
        m_fd = -1;
    }

private:
    int m_fd{ -1 };
};
#endif  // ifndef _MSC_VER


using unique_file_ptr = std::unique_ptr<std::FILE, std::function<void ( std::FILE* )> >;

inline unique_file_ptr
make_unique_file_ptr( std::FILE* file )
{
    return unique_file_ptr( file, []( auto* ownedFile ){
        if ( ownedFile != nullptr ) {
            std::fclose( ownedFile );
        } } );
}

inline unique_file_ptr
make_unique_file_ptr( char const* const filePath,
                      char const* const mode )
{
    return make_unique_file_ptr( std::fopen( filePath, mode ) );
}

inline unique_file_ptr
make_unique_file_ptr( int         fileDescriptor,
                      char const* mode )
{
    return make_unique_file_ptr( fdopen( fileDescriptor, mode ) );
}



inline unique_file_ptr
throwingOpen( const std::string& filePath,
              const char*        mode )
{
    if ( mode == nullptr ) {
        throw std::invalid_argument( "Mode must be a C-String and not null!" );
    }

    auto file = make_unique_file_ptr( filePath.c_str(), mode );
    if ( file == nullptr ) {
        std::stringstream msg;
        msg << "Opening file '" << filePath << "' with mode '" << mode << "' failed!";
        throw std::invalid_argument( std::move( msg ).str() );
    }

    return file;
}


inline unique_file_ptr
throwingOpen( int         fileDescriptor,
              const char* mode )
{
    if ( mode == nullptr ) {
        throw std::invalid_argument( "Mode must be a C-String and not null!" );
    }

    auto file = make_unique_file_ptr( fileDescriptor, mode );
    if ( file == nullptr ) {
        std::stringstream msg;
        msg << "Opening file descriptor " << fileDescriptor << " with mode '" << mode << "' failed!";
        throw std::invalid_argument( std::move( msg ).str() );
    }

    return file;
}


/** dup is not strong enough to be able to independently seek in the old and the dup'ed fd! */
[[nodiscard]] std::string
fdFilePath( int fileDescriptor )
{
    std::stringstream filename;
    filename << "/dev/fd/" << fileDescriptor;
    return filename.str();
}


#ifndef __APPLE_CC__  // Missing std::filesytem::path support in wheels
[[nodiscard]] std::string
findParentFolderContaining( const std::string& folder,
                            const std::string& relativeFilePath )
{
    auto parentFolder = std::filesystem::absolute( folder );
    while ( !parentFolder.empty() )
    {
        const auto filePath = parentFolder / relativeFilePath;
        if ( std::filesystem::exists( filePath ) ) {
            return parentFolder.string();
        }

        if ( parentFolder.parent_path() == parentFolder ) {
            break;
        }
        parentFolder = parentFolder.parent_path();
    }

    return {};
}
#endif


#if defined( HAVE_VMSPLICE )

#include <algorithm>

/**
 * Short overview of syscalls that optimize copies by instead copying full page pointers into the
 * pipe buffers inside the kernel:
 * - splice: <fd (pipe or not)> <-> <pipe>
 * - vmsplice: memory -> <pipe>
 * - mmap: <fd> -> memory
 * - sendfile: <fd that supports mmap> -> <fd (before Linux 2.6.33 (2010-02-24) it had to be a socket fd)>
 *
 * I think the underlying problem with wrong output data for small chunk sizes
 * is that vmsplice is not as "synchronous" as I thought it to be:
 *
 * https://lwn.net/Articles/181169/
 *
 *  - Determining whether it is safe to write to a vmspliced buffer is
 *    suggested to be done implicitly by splicing more than the maximum
 *    number of pages that can be inserted into the pipe buffer.
 *    That number was supposed to be queryable with fcntl F_GETPSZ.
 *    -> This is probably why I didn't notice problems with larger chunk
 *       sizes.
 *  - Even that might not be safe enough when there are multiple pipe
 *    buffers.
 *
 * https://stackoverflow.com/questions/70515745/how-do-i-use-vmsplice-to-correctly-output-to-a-pipe
 * https://codegolf.stackexchange.com/questions/215216/high-throughput-fizz-buzz/239848#239848
 *
 *  - the safest way to use vmsplice seems to be mmap -> vmplice with
 *    SPLICE_F_GIFT -> munmap. munmap can be called directly after the
 *    return from vmplice and this works in a similar way to aio_write
 *    but actually a lot faster.
 *
 * I think using std::vector with vmsplice is NOT safe when it is
 * destructed too soon! The problem here is that the memory is probably not
 * returned to the system, which would be fine, but is actually reused by
 * the c/C++ standard library's implementation of malloc/free/new/delete:
 *
 * https://stackoverflow.com/a/1119334
 *
 *  - In many malloc/free implementations, free does normally not return
 *    the memory to the operating system (or at least only in rare cases).
 *    [...] Free will put the memory block in its own free block list.
 *    Normally it also tries to meld together adjacent blocks in the
 *    address space.
 *
 * https://mazzo.li/posts/fast-pipes.html
 * https://github.com/bitonic/pipes-speed-test.git
 *
 *  - Set pipe size and double buffer. (Similar to the lwn article
 *    but instead of querying the pipe size, it is set.)
 *  - fcntl(STDOUT_FILENO, F_SETPIPE_SZ, options.pipe_size);
 *
 * I think I will have to implement a container with a custom allocator
 * that uses mmap and munmap to get back my vmsplice speeds :/(.
 * Or maybe try setting the pipe buffer size to some forced value and
 * then only free the last data after pipe size more has been written.
 *
 * @note Throws if some splice calls were successful followed by an unsucessful one before finishing.
 * @return true if successful and false if it could not be spliced from the beginning, e.g., because the file
 *         descriptor is not a pipe.
 */
[[nodiscard]] bool
writeAllSpliceUnsafe( [[maybe_unused]] const int         outputFileDescriptor,
                      [[maybe_unused]] const void* const dataToWrite,
                      [[maybe_unused]] const size_t      dataToWriteSize )
{
    ::iovec dataToSplice{};
    /* The const_cast should be safe because vmsplice should not modify the input data. */
    dataToSplice.iov_base = const_cast<void*>( reinterpret_cast<const void*>( dataToWrite ) );
    dataToSplice.iov_len = dataToWriteSize;
    while ( dataToSplice.iov_len > 0 ) {
        const auto nBytesWritten = ::vmsplice( outputFileDescriptor, &dataToSplice, 1, /* flags */ 0 );
        if ( nBytesWritten < 0 ) {
            if ( dataToSplice.iov_len == dataToWriteSize ) {
                return false;
            }
            std::cerr << "error: " << errno << "\n";
            throw std::runtime_error( "Failed to write to pipe" );
        }
        dataToSplice.iov_base = reinterpret_cast<char*>( dataToSplice.iov_base ) + nBytesWritten;
        dataToSplice.iov_len -= nBytesWritten;
    }
    return true;
}


[[nodiscard]] bool
writeAllSpliceUnsafe( [[maybe_unused]] const int                   outputFileDescriptor,
                      [[maybe_unused]] const std::vector<::iovec>& dataToWrite )
{
    for ( size_t i = 0; i < dataToWrite.size(); ) {
        const auto segmentCount = std::min( static_cast<size_t>( IOV_MAX ), dataToWrite.size() - i );
        auto nBytesWritten = ::vmsplice( outputFileDescriptor, &dataToWrite[i], segmentCount, /* flags */ 0 );

        if ( nBytesWritten < 0 ) {
            if ( i == 0 ) {
                return false;
            }

            std::stringstream message;
            message << "Failed to write all bytes because of: " << strerror( errno ) << " (" << errno << ")";
            throw std::runtime_error( std::move( message.str() ) );
        }

        /* Skip over buffers that were written fully. */
        for ( ; ( i < dataToWrite.size() ) && ( dataToWrite[i].iov_len <= static_cast<size_t>( nBytesWritten ) ); ++i ) {
            nBytesWritten -= dataToWrite[i].iov_len;
        }

        /* Write out last partially written buffer if necessary so that we can resumefull vectorized writing
         * from the next iovec buffer. */
        if ( ( i < dataToWrite.size() ) && ( nBytesWritten > 0 ) ) {
            const auto& iovBuffer = dataToWrite[i];

            assert( iovBuffer.iov_len < static_cast<size_t>( nBytesWritten ) );
            const auto size = iovBuffer.iov_len - nBytesWritten;

            const auto remainingData = reinterpret_cast<char*>( iovBuffer.iov_base ) + nBytesWritten;
            if ( !writeAllSpliceUnsafe( outputFileDescriptor, remainingData, size ) ) {
                throw std::runtime_error( "Failed to write to pipe subsequently." );
            }
            ++i;
        }
    }

    return true;
}


/**
 * Keeps shared pointers to spliced objects until an amount of bytes equal to the pipe buffer size
 * has been spliced into the pipe.
 * It implements a singleton-like (singleton per file descriptor) interface as a performance optimization.
 * Without a global ledger, the effectively held back objects would be overestimated by the number of actual ledgers.
 */
class SpliceVault
{
public:
    using VaultLock = std::unique_lock<AtomicMutex>;

public:
    [[nodiscard]] static std::pair<SpliceVault*, VaultLock>
    getInstance( int fileDescriptor )
    {
        static AtomicMutex mutex;
        static std::unordered_map<int, std::unique_ptr<SpliceVault> > vaults;

        const std::scoped_lock lock{ mutex };
        auto vault = vaults.find( fileDescriptor );
        if ( vault == vaults.end() ) {
            /* try_emplace cannot be used because the SpliceVault constructor is private. */
            vault = vaults.emplace( fileDescriptor,
                                    std::unique_ptr<SpliceVault>( new SpliceVault( fileDescriptor ) ) ).first;
        }
        return std::make_pair( vault->second.get(), vault->second->lock() );
    }

    /**
     * @param dataToWrite A pointer to the start of the data to write. This pointer should be part of @p splicedData!
     * @param splicedData This owning shared pointer will be stored until enough other data has been spliced into
     *                    the pipe.
     */
    template<typename T>
    [[nodiscard]] bool
    splice( const void* const         dataToWrite,
            size_t const              dataToWriteSize,
            const std::shared_ptr<T>& splicedData )
    {
        if ( ( m_pipeBufferSize < 0 )
             || !writeAllSpliceUnsafe( m_fileDescriptor, dataToWrite, dataToWriteSize ) ) {
            return false;
        }

        account( splicedData, dataToWriteSize );
        return true;
    }

    /**
     * Overload that works for iovec structures directly.
     */
    template<typename T>
    [[nodiscard]] bool
    splice( const std::vector<::iovec>& buffersToWrite,
            const std::shared_ptr<T>&   splicedData )
    {
        if ( ( m_pipeBufferSize < 0 )
             || !writeAllSpliceUnsafe( m_fileDescriptor, buffersToWrite ) ) {
            return false;
        }

        const auto dataToWriteSize = std::accumulate(
            buffersToWrite.begin(), buffersToWrite.end(), size_t( 0 ),
            [] ( size_t sum, const auto& buffer ) { return sum + buffer.iov_len; } );

        account( splicedData, dataToWriteSize );
        return true;
    }


private:
    template<typename T>
    void
    account( const std::shared_ptr<T>& splicedData,
             size_t const              dataToWriteSize )
    {
        m_totalSplicedBytes += dataToWriteSize;
        /* Append written size to last shared pointer if it is the same one or add a new data set. */
        if ( !m_splicedData.empty() && ( std::get<1>( m_splicedData.back() ) == splicedData.get() ) ) {
            std::get<2>( m_splicedData.back() ) += dataToWriteSize;
        } else {
            m_splicedData.emplace_back( splicedData, splicedData.get(), dataToWriteSize );
        }

        /* Never fully clear the shared pointers even if the size of the last is larger than the pipe buffer
         * because part of that last large chunk will still be in the pipe buffer! */
        while ( !m_splicedData.empty()
                && ( m_totalSplicedBytes - std::get<2>( m_splicedData.front() )
                     >= static_cast<size_t>( m_pipeBufferSize ) ) ) {
            m_totalSplicedBytes -= std::get<2>( m_splicedData.front() );
            m_splicedData.pop_front();
        }
    }

    explicit
    SpliceVault( int fileDescriptor ) :
        m_fileDescriptor( fileDescriptor ),
        m_pipeBufferSize( fcntl( fileDescriptor, F_GETPIPE_SZ ) )
    {}

    [[nodiscard]] VaultLock
    lock()
    {
        return VaultLock( m_mutex );
    }

private:
    const int m_fileDescriptor;
    /** We are assuming here that the pipe buffer size does not change to avoid frequent calls to fcntl. */
    const int m_pipeBufferSize;

    /**
     * Contains shared_ptr to extend lifetime and amount of bytes that have been spliced to determine
     * when the shared_ptr can be removed from this list.
     */
    std::deque<std::tuple</* packed RAII resource */ std::any,
                          /* raw pointer of RAII resource for comparison */ const void*,
                          /* spliced bytes */ size_t> > m_splicedData;
    /**
     * This data is redundant but helps to avoid O(N) recalculation of this value from @ref m_splicedData.
     */
    size_t m_totalSplicedBytes{ 0 };

    AtomicMutex m_mutex;
};
#endif  // HAVE_VMSPLICE


/**
 * Posix write is not guaranteed to write everything and in fact was encountered to not write more than
 * 0x7ffff000 (2'147'479'552) B. To avoid this, it has to be looped over.
 */
void
writeAllToFd( const int         outputFileDescriptor,
              const void* const dataToWrite,
              const uint64_t    dataToWriteSize )
{
    for ( uint64_t nTotalWritten = 0; nTotalWritten < dataToWriteSize; ) {
        const auto currentBufferPosition =
            reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( dataToWrite ) + nTotalWritten );
        const auto nBytesWritten = ::write( outputFileDescriptor,
                                            currentBufferPosition,
                                            dataToWriteSize - nTotalWritten );
        if ( nBytesWritten <= 0 ) {
            std::stringstream message;
            message << "Unable to write all data to the given file descriptor. Wrote " << nTotalWritten << " out of "
                    << dataToWriteSize << " (" << strerror( errno ) << ").";
            throw std::runtime_error( std::move( message ).str() );
        }
        nTotalWritten += static_cast<uint64_t>( nBytesWritten );
    }
}


#ifdef HAVE_IOVEC
void
pwriteAllToFd( const int         outputFileDescriptor,
               const void* const dataToWrite,
               const uint64_t    dataToWriteSize,
               const uint64_t    fileOffset )
{
    for ( uint64_t nTotalWritten = 0; nTotalWritten < dataToWriteSize; ) {
        const auto currentBufferPosition =
            reinterpret_cast<const void*>( reinterpret_cast<uintptr_t>( dataToWrite ) + nTotalWritten );
        const auto nBytesWritten = ::pwrite( outputFileDescriptor,
                                             currentBufferPosition,
                                             dataToWriteSize - nTotalWritten,
                                             fileOffset + nTotalWritten );
        if ( nBytesWritten <= 0 ) {
            std::stringstream message;
            message << "Unable to write all data to the given file descriptor. Wrote " << nTotalWritten << " out of "
                    << dataToWriteSize << " (" << strerror( errno ) << ").";
            throw std::runtime_error( std::move( message ).str() );
        }

        nTotalWritten += static_cast<uint64_t>( nBytesWritten );
    }
}


void
writeAllToFdVector( const int                   outputFileDescriptor,
                    const std::vector<::iovec>& dataToWrite )
{
    for ( size_t i = 0; i < dataToWrite.size(); ) {
        const auto segmentCount = std::min( static_cast<size_t>( IOV_MAX ), dataToWrite.size() - i );
        auto nBytesWritten = ::writev( outputFileDescriptor, &dataToWrite[i], segmentCount );

        if ( nBytesWritten < 0 ) {
            std::stringstream message;
            message << "Failed to write all bytes because of: " << strerror( errno ) << " (" << errno << ")";
            throw std::runtime_error( std::move( message.str() ) );
        }

        /* Skip over buffers that were written fully. */
        for ( ; ( i < dataToWrite.size() ) && ( dataToWrite[i].iov_len <= static_cast<size_t>( nBytesWritten ) ); ++i ) {
            nBytesWritten -= dataToWrite[i].iov_len;
        }

        /* Write out last partially written buffer if necessary so that we can resume full vectorized writing
         * from the next iovec buffer. */
        if ( ( i < dataToWrite.size() ) && ( nBytesWritten > 0 ) ) {
            const auto& iovBuffer = dataToWrite[i];

            assert( iovBuffer.iov_len < static_cast<size_t>( nBytesWritten ) );
            const auto remainingSize = iovBuffer.iov_len - nBytesWritten;
            const auto remainingData = reinterpret_cast<char*>( iovBuffer.iov_base ) + nBytesWritten;
            writeAllToFd( outputFileDescriptor, remainingData, remainingSize );

            ++i;
        }
    }
}


void
pwriteAllToFdVector( const int                   outputFileDescriptor,
                     const std::vector<::iovec>& dataToWrite,
                     size_t                      fileOffset )
{
    for ( size_t i = 0; i < dataToWrite.size(); ) {
        const auto segmentCount = std::min( static_cast<size_t>( IOV_MAX ), dataToWrite.size() - i );
        auto nBytesWritten = ::pwritev( outputFileDescriptor, &dataToWrite[i], segmentCount, fileOffset );

        if ( nBytesWritten < 0 ) {
            std::stringstream message;
            message << "Failed to write all bytes because of: " << strerror( errno ) << " (" << errno << ")";
            throw std::runtime_error( std::move( message.str() ) );
        }

        fileOffset += nBytesWritten;

        /* Skip over buffers that were written fully. */
        for ( ; ( i < dataToWrite.size() ) && ( dataToWrite[i].iov_len <= static_cast<size_t>( nBytesWritten ) ); ++i ) {
            nBytesWritten -= dataToWrite[i].iov_len;
        }

        /* Write out last partially written buffer if necessary so that we can resume full vectorized writing
         * from the next iovec buffer. */
        if ( ( i < dataToWrite.size() ) && ( nBytesWritten > 0 ) ) {
            const auto& iovBuffer = dataToWrite[i];

            assert( iovBuffer.iov_len < static_cast<size_t>( nBytesWritten ) );
            const auto remainingSize = iovBuffer.iov_len - nBytesWritten;
            const auto remainingData = reinterpret_cast<char*>( iovBuffer.iov_base ) + nBytesWritten;
            pwriteAllToFd( outputFileDescriptor, remainingData, remainingSize, fileOffset );
            fileOffset += remainingSize;

            ++i;
        }
    }
}
#endif  // HAVE_IOVEC


void
writeAll( const int         outputFileDescriptor,
          void* const       outputBuffer,
          const void* const dataToWrite,
          const uint64_t    dataToWriteSize )
{
    if ( dataToWriteSize == 0 ) {
        return;
    }

    if ( outputFileDescriptor >= 0 ) {
        writeAllToFd( outputFileDescriptor, dataToWrite, dataToWriteSize );
    }

    if ( outputBuffer != nullptr ) {
        if ( dataToWriteSize > std::numeric_limits<size_t>::max() ) {
            throw std::invalid_argument( "Too much data to write!" );
        }
        std::memcpy( outputBuffer, dataToWrite, dataToWriteSize );
    }
}
