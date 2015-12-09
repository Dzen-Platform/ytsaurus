#pragma once

#include "public.h"
#include "chunk_reader.h"

#include <yt/core/actions/future.h>

#include <yt/core/compression/public.h>

#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! For a sequence of block indexes, fetches and uncompresses these blocks in the given order.
/*!
 *  Internally, blocks are prefetched obeying a given memory limit.
 */
class TSequentialReader
    : public TRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(volatile i64, UncompressedDataSize);
    DEFINE_BYVAL_RO_PROPERTY(volatile i64, CompressedDataSize);

public:
    struct TBlockInfo
    {
        int Index;
        int UncompressedDataSize;

        TBlockInfo()
            : Index(-1)
            , UncompressedDataSize(0)
        { }

        TBlockInfo(int index, int uncompressedDataSize)
            : Index(index)
            , UncompressedDataSize(uncompressedDataSize)
        { }
    };

    TSequentialReader(
        TSequentialReaderConfigPtr config,
        std::vector<TBlockInfo> blockInfos,
        IChunkReaderPtr chunkReader,
        IBlockCachePtr blockCache,
        NCompression::ECodec codecId);

    //! Returns |true| if the current block is not the last one.
    bool HasMoreBlocks() const;

    //! Asynchronously fetches the next block.
    /*!
     *  It is not allowed to ask for the next block until the previous one is retrieved.
     *  If an error occurs during fetching then the whole session is failed.
     */
    TFuture<void> FetchNextBlock();

    //! Returns the current block.
    /*!
     *  The block must have been already fetched by #FetchNextBlock.
     */
    TSharedRef GetCurrentBlock();

    //! Returns a asynchronous flag that becomes set when all
    //! blocks are fetched.
    TFuture<void> GetFetchingCompletedEvent();

private:
    void FetchNextGroup();

    void RequestBlocks(
        const std::vector<int>& windowIndexes,
        const std::vector<int>& blockIndexes,
        i64 uncompressedSize,
        const TError& error);

    void OnGotBlocks(
        const std::vector<int>& windowIndexes,
        const std::vector<int>& blockIndexes,
        const TErrorOr<std::vector<TSharedRef>>& blocksOrError);

    void DecompressBlocks(
        const std::vector<int>& windowIndexes,
        const std::vector<TSharedRef>& compressedBlocks);

    void MarkFailedBlocks(
        const std::vector<int>& windowIndexes,
        const TError& error);


    const TSequentialReaderConfigPtr Config_;
    const std::vector<TBlockInfo> BlockInfos_;
    const IChunkReaderPtr ChunkReader_;
    const IBlockCachePtr BlockCache_;

    const IInvokerPtr CompressionInvoker_;

    struct TWindowSlot
    {
        TPromise<TSharedRef> Block = NewPromise<TSharedRef>();
        bool Cached = false;
    };

    std::vector<TWindowSlot> Window_;

    NConcurrency::TAsyncSemaphore AsyncSemaphore_;

    int FirstReadyWindowIndex_ = -1;
    int FirstUnfetchedWindowIndex_ = 0;

    TPromise<void> FetchingComplete_ = NewPromise<void>();

    NCompression::ICodec* const Codec_;

    NLogging::TLogger Logger;

};

DEFINE_REFCOUNTED_TYPE(TSequentialReader)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
