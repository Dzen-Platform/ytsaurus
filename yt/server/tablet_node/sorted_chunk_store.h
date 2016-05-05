#pragma once

#include "public.h"
#include "store_detail.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/versioned_row.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TSortedChunkStore
    : public TChunkStoreBase
    , public TSortedStoreBase
{
public:
    TSortedChunkStore(
        TTabletManagerConfigPtr config,
        const TStoreId& id,
        TTablet* tablet,
        NChunkClient::IBlockCachePtr blockCache,
        NDataNode::TChunkRegistryPtr chunkRegistry = nullptr,
        NDataNode::TChunkBlockManagerPtr chunkBlockManager = nullptr,
        NApi::IClientPtr client = nullptr,
        const TNullable<NNodeTrackerClient::TNodeDescriptor>& localDescriptor = Null);
    ~TSortedChunkStore();

    // IStore implementation.
    virtual TSortedChunkStorePtr AsSortedChunk() override;

    // IChunkStore implementation.
    virtual EStoreType GetType() const override;

    virtual EInMemoryMode GetInMemoryMode() const override;
    virtual void SetInMemoryMode(EInMemoryMode mode) override;

    virtual void Preload(TInMemoryChunkDataPtr chunkData) override;

    // ISortedStore implementation.
    virtual TOwningKey GetMinKey() const override;
    virtual TOwningKey GetMaxKey() const override;

    virtual TTimestamp GetMinTimestamp() const override;
    virtual TTimestamp GetMaxTimestamp() const override;

    virtual NTableClient::IVersionedReaderPtr CreateReader(
        const TTabletSnapshotPtr& tabletSnapshot,
        TOwningKey lowerKey,
        TOwningKey upperKey,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter,
        const TWorkloadDescriptor& workloadDescriptor) override;

    virtual NTableClient::IVersionedReaderPtr CreateReader(
        const TTabletSnapshotPtr& tabletSnapshot,
        const TSharedRange<TKey>& keys,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter,
        const TWorkloadDescriptor& workloadDescriptor) override;

    virtual void CheckRowLocks(
        TUnversionedRow row,
        TTransaction* transaction,
        ui32 lockMask) override;

private:
    class TPreloadedBlockCache;
    using TPreloadedBlockCachePtr = TIntrusivePtr<TPreloadedBlockCache>;

    // Cached for fast retrieval from ChunkMeta_.
    TOwningKey MinKey_;
    TOwningKey MaxKey_;

    NTableClient::TCachedVersionedChunkMetaPtr CachedVersionedChunkMeta_;

    TPreloadedBlockCachePtr PreloadedBlockCache_;

    EInMemoryMode InMemoryMode_ = EInMemoryMode::None;

    const NTableClient::TKeyComparer KeyComparer_;
    const bool RequireChunkPreload_;


    NTableClient::IVersionedReaderPtr CreateCacheBasedReader(
        const TSharedRange<TKey>& keys,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter);
    NTableClient::IVersionedReaderPtr CreateCacheBasedReader(
        TOwningKey lowerKey,
        TOwningKey upperKey,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter);

    NTableClient::TCachedVersionedChunkMetaPtr PrepareCachedVersionedChunkMeta(
        NChunkClient::IChunkReaderPtr chunkReader);

    virtual NChunkClient::IBlockCachePtr GetBlockCache() override;

    virtual void PrecacheProperties() override;

    bool ValidateBlockCachePreloaded();

    ISortedStorePtr GetSortedBackingStore();

};

DEFINE_REFCOUNTED_TYPE(TSortedChunkStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
