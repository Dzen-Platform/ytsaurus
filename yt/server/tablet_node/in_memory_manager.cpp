#include "in_memory_manager.h"
#include "private.h"
#include "chunk_store.h"
#include "config.h"
#include "slot_manager.h"
#include "store_manager.h"
#include "tablet.h"
#include "tablet_manager.h"
#include "tablet_slot.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/misc/memory_usage_tracker.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/dispatcher.h>

#include <yt/core/compression/codec.h>

#include <yt/core/concurrency/async_semaphore.h>
#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/rw_spinlock.h>
#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/thread_affinity.h>

namespace NYT {
namespace NTabletNode {

using namespace NHydra;
using namespace NConcurrency;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

const auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

EBlockType InMemoryModeToBlockType(EInMemoryMode mode)
{
    switch (mode) {
        case EInMemoryMode::Compressed:
            return EBlockType::CompressedData;

        case EInMemoryMode::Uncompressed:
            return EBlockType::UncompressedData;

        case EInMemoryMode::None:
            return EBlockType::None;

        default:
            YUNREACHABLE();
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TInMemoryManager::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TInMemoryManagerConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , CompressionInvoker_(CreateFixedPriorityInvoker(
            NChunkClient::TDispatcher::Get()->GetCompressionPoolInvoker(),
            Config_->WorkloadDescriptor.GetPriority()))
        , PreloadSemaphore_(Config_->MaxConcurrentPreloads)
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->SubscribeScanSlot(BIND(&TImpl::ScanSlot, MakeStrong(this)));
    }

    IBlockCachePtr CreateInterceptingBlockCache(EInMemoryMode mode)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return New<TInterceptingBlockCache>(this, mode);
    }

    TInMemoryChunkDataPtr EvictInterceptedChunkData(const TChunkId& chunkId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TWriterGuard guard(InterceptedDataSpinLock_);

        auto it = ChunkIdToData_.find(chunkId);
        if (it == ChunkIdToData_.end()) {
            return nullptr;
        }

        auto data = it->second;
        ChunkIdToData_.erase(it);

        LOG_INFO("Intercepted chunk data evicted (ChunkId: %v, Mode: %v)",
            chunkId,
            data->InMemoryMode);

        return data;
    }

    void FinalizeChunk(
        const TChunkId& chunkId,
        const TChunkMeta& chunkMeta,
        const TTabletSnapshotPtr& tabletSnapshot)
    {
        TInMemoryChunkDataPtr data;
        auto mode = tabletSnapshot->Config->InMemoryMode;
        if (mode == EInMemoryMode::None) {
            return;
        }

        {
            TWriterGuard guard(InterceptedDataSpinLock_);
            auto it = ChunkIdToData_.find(chunkId);
            if (it != ChunkIdToData_.end()) {
                data = it->second;
            }
        }

        if (!data) {
            LOG_INFO("Cannot find intercepted chunk (TabletId: %v, Mode: %v, ChunkId: %v)",
                tabletSnapshot->TabletId,
                mode,
                chunkId);
            return;
        }

        YCHECK(data->InMemoryMode == mode);

        FinalizeChunkData(data, chunkId, chunkMeta, tabletSnapshot);
    }

private:
    const TInMemoryManagerConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    const IInvokerPtr CompressionInvoker_;

    TAsyncSemaphore PreloadSemaphore_;

    TReaderWriterSpinLock InterceptedDataSpinLock_;
    yhash_map<TChunkId, TInMemoryChunkDataPtr> ChunkIdToData_;


    void FinalizeChunkData(
        TInMemoryChunkDataPtr data,
        const TChunkId& chunkId,
        const TChunkMeta& chunkMeta,
        const TTabletSnapshotPtr& tabletSnapshot)
    {
        data->ChunkMeta = TCachedVersionedChunkMeta::Create(
            chunkId,
            chunkMeta,
            tabletSnapshot->Schema);

        if (tabletSnapshot->HashTableSize > 0) {
            data->LookupHashTable = CreateChunkLookupHashTable(
                data->Blocks,
                data->ChunkMeta,
                tabletSnapshot->RowKeyComparer);
        }
    }

    void ScanSlot(TTabletSlotPtr slot)
    {
        if (IsMemoryLimitExceeded())
            return;

        if (slot->GetAutomatonState() != EPeerState::Leading)
            return;

        auto tabletManager = slot->GetTabletManager();
        for (const auto& pair : tabletManager->Tablets()) {
            auto* tablet = pair.second;
            ScanTablet(tablet);
        }
    }

    void ScanTablet(TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::Mounted)
            return;

        auto storeManager = tablet->GetStoreManager();
        while (true) {
            auto store = storeManager->PeekStoreForPreload();
            if (!store)
                break;
            if (!ScanStore(tablet, store))
                break;
        }
    }

    bool ScanStore(TTablet* tablet, TChunkStorePtr store)
    {
        auto guard = TAsyncSemaphoreGuard::TryAcquire(&PreloadSemaphore_);
        if (!guard) {
            return false;
        }

        auto future =
            BIND(
                &TImpl::PreloadStore,
                MakeStrong(this),
                Passed(std::move(guard)),
                tablet,
                store)
            .AsyncVia(tablet->GetEpochAutomatonInvoker())
            .Run();

        auto storeManager = tablet->GetStoreManager();
        storeManager->BeginStorePreload(store, future);
        return true;
    }

    void PreloadStore(
        TAsyncSemaphoreGuard /*guard*/,
        TTablet* tablet,
        TChunkStorePtr store)
    {
        NLogging::TLogger Logger(TabletNodeLogger);
        Logger.AddTag("TabletId: %v, StoreId: %v",
            tablet->GetId(),
            store->GetId());

        auto storeManager = tablet->GetStoreManager();
        try {
            GuardedPreloadStore(tablet, store, Logger);
            storeManager->EndStorePreload(store);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error preloading tablet store, backing off");
            storeManager->BackoffStorePreload(store);
        }

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->UpdateTabletSnapshot(tablet);
    }

    void GuardedPreloadStore(
        TTablet* tablet,
        TChunkStorePtr store,
        const NLogging::TLogger& Logger)
    {
        if (IsMemoryLimitExceeded())
            return;

        auto mode = tablet->GetConfig()->InMemoryMode;
        if (mode == EInMemoryMode::None)
            return;

        auto reader = store->GetChunkReader(TWorkloadDescriptor(EWorkloadCategory::SystemTabletPreload));

        LOG_INFO("Store preload started");

        auto meta = WaitFor(reader->GetMeta())
            .ValueOrThrow();

        auto miscExt = GetProtoExtension<TMiscExt>(meta.extensions());
        auto blocksExt = GetProtoExtension<TBlocksExt>(meta.extensions());

        auto codecId = NCompression::ECodec(miscExt.compression_codec());
        auto* codec = NCompression::GetCodec(codecId);

        auto chunkData = New<TInMemoryChunkData>();
        chunkData->InMemoryMode = mode;

        int startBlockIndex = 0;
        int totalBlockCount = blocksExt.blocks_size();
        while (startBlockIndex < totalBlockCount) {
            LOG_DEBUG("Started reading chunk blocks (FirstBlock: %v)",
                startBlockIndex);

            auto asyncResult = reader->ReadBlocks(startBlockIndex, totalBlockCount - startBlockIndex);
            auto compressedBlocks = WaitFor(asyncResult)
                .ValueOrThrow();

            int readBlockCount = compressedBlocks.size();
            LOG_DEBUG("Finished reading chunk blocks (Blocks: %v-%v)",
                startBlockIndex,
                startBlockIndex + readBlockCount - 1);

            std::vector<TSharedRef> cachedBlocks;
            switch (mode) {
                case EInMemoryMode::Compressed:
                    cachedBlocks = std::move(compressedBlocks);
                    break;

                case EInMemoryMode::Uncompressed: {
                    LOG_DEBUG("Decompressing chunk blocks (Blocks: %v-%v)",
                        startBlockIndex,
                        startBlockIndex + readBlockCount - 1);

                    std::vector<TFuture<TSharedRef>> asyncUncompressedBlocks;
                    for (const auto& compressedBlock : compressedBlocks) {
                        asyncUncompressedBlocks.push_back(
                            BIND([=] () { return codec->Decompress(compressedBlock); })
                                .AsyncVia(CompressionInvoker_)
                                .Run());
                    }

                    cachedBlocks = WaitFor(Combine(asyncUncompressedBlocks))
                        .ValueOrThrow();
                    break;
                }

                default:
                    YUNREACHABLE();
            }

            chunkData->Blocks.insert(chunkData->Blocks.end(), cachedBlocks.begin(), cachedBlocks.end());

            startBlockIndex += readBlockCount;
        }

        FinalizeChunkData(chunkData, store->GetId(), meta, tablet->GetSnapshot());

        store->Preload(chunkData);

        LOG_INFO("Store preload completed (LookupHashTable: %v)", static_cast<bool>(chunkData->LookupHashTable));
    }

    class TInterceptingBlockCache
        : public IBlockCache
    {
    public:
        TInterceptingBlockCache(
            TImplPtr owner,
            EInMemoryMode mode)
            : Owner_(owner)
            , Mode_(mode)
            , BlockType_(InMemoryModeToBlockType(Mode_))
        { }

        ~TInterceptingBlockCache()
        {
            for (const auto& chunkId : ChunkIds_) {
                TDelayedExecutor::Submit(
                    BIND(IgnoreResult(&TImpl::EvictInterceptedChunkData), Owner_, chunkId),
                    Owner_->Config_->InterceptedDataRetentionTime);
            }
        }

        virtual void Put(
            const TBlockId& id,
            EBlockType type,
            const TSharedRef& block,
            const TNullable<NNodeTrackerClient::TNodeDescriptor>& /*source*/) override
        {
            if (type != BlockType_)
                return;

            if (Owner_->IsMemoryLimitExceeded()) {
                TGuard<TSpinLock> guard(SpinLock_);
                Dropped_ = true;
                Owner_->DropChunkData(id.ChunkId);
                return;
            }

            TGuard<TSpinLock> guard(SpinLock_);

            if (Dropped_)
                return;

            auto it = ChunkIds_.find(id.ChunkId);
            TInMemoryChunkDataPtr data;
            if (it == ChunkIds_.end()) {
                data = Owner_->CreateChunkData(id.ChunkId, Mode_);
                YCHECK(ChunkIds_.insert(id.ChunkId).second);
            } else {
                data = Owner_->GetChunkData(id.ChunkId, Mode_);
            }

            if (data->Blocks.size() <= id.BlockIndex) {
                data->Blocks.resize(id.BlockIndex + 1);
            }
            data->Blocks[id.BlockIndex] = block;

            YCHECK(!data->ChunkMeta);
        }

        virtual TSharedRef Find(
            const TBlockId& /*id*/,
            EBlockType /*type*/) override
        {
            return TSharedRef();
        }

        virtual EBlockType GetSupportedBlockTypes() const override
        {
            return BlockType_;
        }

    private:
        const TImplPtr Owner_;
        const EInMemoryMode Mode_;
        const EBlockType BlockType_;

        TSpinLock SpinLock_;
        yhash_set<TChunkId> ChunkIds_;
        bool Dropped_ = false;
    };

    TInMemoryChunkDataPtr GetChunkData(const TChunkId& chunkId, EInMemoryMode mode)
    {
        TReaderGuard guard(InterceptedDataSpinLock_);

        auto it = ChunkIdToData_.find(chunkId);
        YCHECK(it != ChunkIdToData_.end());
        auto data = it->second;
        YCHECK(data->InMemoryMode == mode);
        return data;
    }

    TInMemoryChunkDataPtr CreateChunkData(const TChunkId& chunkId, EInMemoryMode mode)
    {
        TWriterGuard guard(InterceptedDataSpinLock_);

        auto chunkData = New<TInMemoryChunkData>();
        chunkData->InMemoryMode = mode;

        // Replace the old data, if any, by a new one.
        ChunkIdToData_[chunkId] = chunkData;

        LOG_INFO("Intercepted chunk data created (ChunkId: %v, Mode: %v)",
            chunkId,
            mode);

        return chunkData;
    }

    void DropChunkData(const TChunkId& chunkId)
    {
        TWriterGuard guard(InterceptedDataSpinLock_);

        if (ChunkIdToData_.erase(chunkId) == 1) {
            LOG_WARNING("Intercepted chunk data dropped due to memory pressure (ChunkId: %v)",
                chunkId);
        }
    }

    bool IsMemoryLimitExceeded() const
    {
        const auto* tracker = Bootstrap_->GetMemoryUsageTracker();
        return tracker->IsExceeded(EMemoryCategory::TabletStatic);
    }

};

////////////////////////////////////////////////////////////////////////////////

TInMemoryManager::TInMemoryManager(
    TInMemoryManagerConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TInMemoryManager::~TInMemoryManager()
{ }

IBlockCachePtr TInMemoryManager::CreateInterceptingBlockCache(EInMemoryMode mode)
{
    return Impl_->CreateInterceptingBlockCache(mode);
}

TInMemoryChunkDataPtr TInMemoryManager::EvictInterceptedChunkData(const TChunkId& chunkId)
{
    return Impl_->EvictInterceptedChunkData(chunkId);
}

void TInMemoryManager::FinalizeChunk(
    const TChunkId& chunkId,
    const TChunkMeta& chunkMeta,
    const TTabletSnapshotPtr& tabletSnapshot)
{
    Impl_->FinalizeChunk(chunkId, chunkMeta, tabletSnapshot);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
