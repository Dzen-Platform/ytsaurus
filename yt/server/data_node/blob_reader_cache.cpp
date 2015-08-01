#include "stdafx.h"
#include "blob_reader_cache.h"
#include "private.h"
#include "config.h"
#include "chunk.h"
#include "location.h"

#include <ytlib/chunk_client/file_reader.h>

#include <core/misc/async_cache.h>

namespace NYT {
namespace NDataNode {

using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

class TBlobReaderCache::TCachedReader
    : public TAsyncCacheValueBase<TChunkId, TCachedReader>
    , public TFileReader
{
public:
    TCachedReader(
        const TChunkId& chunkId,
        const Stroka& fileName,
        bool validateBlockChecksums)
        : TAsyncCacheValueBase<TChunkId, TCachedReader>(chunkId)
        , TFileReader(chunkId, fileName, validateBlockChecksums)
        , ChunkId_(chunkId)
    { }

    virtual TChunkId GetChunkId() const override
    {
        return ChunkId_;
    }

private:
    const TChunkId ChunkId_;

};

////////////////////////////////////////////////////////////////////////////////

class TBlobReaderCache::TImpl
    : public TAsyncSlruCacheBase<TChunkId, TCachedReader>
{
public:
    explicit TImpl(TDataNodeConfigPtr config)
        : TAsyncSlruCacheBase(
            config->BlobReaderCache,
            NProfiling::TProfiler(DataNodeProfiler.GetPathPrefix() + "/block_reader_cache"))
        , Config_(config)
    { }

    TFileReaderPtr GetReader(IChunkPtr chunk)
    {
        YCHECK(chunk->IsReadLockAcquired());

        auto location = chunk->GetLocation();
        const auto& Profiler = location->GetProfiler();

        auto chunkId = chunk->GetId();
        auto cookie = BeginInsert(chunkId);
        if (cookie.IsActive()) {
            auto fileName = chunk->GetFileName();
            LOG_TRACE("Started opening blob chunk reader (LocationId: %v, ChunkId: %v)",
                location->GetId(),
                chunkId);

            PROFILE_TIMING ("/blob_chunk_reader_open_time") {
                try {
                    auto reader = New<TCachedReader>(chunkId, fileName, Config_->ValidateBlockChecksums);
                    reader->Open();
                    cookie.EndInsert(reader);
                } catch (const std::exception& ex) {
                    auto error = TError(
                        NChunkClient::EErrorCode::IOError,
                        "Error opening blob chunk %v",
                        chunkId)
                        << ex;
                    cookie.Cancel(error);
                    chunk->GetLocation()->Disable(error);
                    THROW_ERROR error;
                }
            }

            LOG_TRACE("Finished opening blob chunk reader (LocationId: %v, ChunkId: %v)",
                chunk->GetLocation()->GetId(),
                chunkId);
        }

        return cookie.GetValue().Get().ValueOrThrow();
    }

    void EvictReader(IChunk* chunk)
    {
        TAsyncSlruCacheBase::TryRemove(chunk->GetId());
    }

private:
    const TDataNodeConfigPtr Config_;


    virtual void OnAdded(const TCachedReaderPtr& reader) override
    {
        LOG_TRACE("Block chunk reader added to cache (ChunkId: %v)",
            reader->GetKey());
    }

    virtual void OnRemoved(const TCachedReaderPtr& reader) override
    {
        LOG_TRACE("Block chunk reader removed from cache (ChunkId: %v)",
            reader->GetKey());
    }

};

////////////////////////////////////////////////////////////////////////////////

TBlobReaderCache::TBlobReaderCache(TDataNodeConfigPtr config)
    : Impl_(New<TImpl>(config))
{ }

TBlobReaderCache::~TBlobReaderCache()
{ }

TFileReaderPtr TBlobReaderCache::GetReader(IChunkPtr chunk)
{
    return Impl_->GetReader(chunk);
}

void TBlobReaderCache::EvictReader(IChunk* chunk)
{
    Impl_->EvictReader(chunk);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
