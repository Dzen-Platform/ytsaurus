#include "store_manager_detail.h"
#include "private.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "store.h"
#include "in_memory_manager.h"
#include "config.h"

#include <yt/server/tablet_node/tablet_manager.pb.h>

namespace NYT {
namespace NTabletNode {

using namespace NApi;
using namespace NChunkClient;

using NTabletNode::NProto::TAddStoreDescriptor;

////////////////////////////////////////////////////////////////////////////////

TStoreManagerBase::TStoreManagerBase(
    TTabletManagerConfigPtr config,
    TTablet* tablet,
    ITabletContext* tabletContext,
    NHydra::IHydraManagerPtr hydraManager,
    TInMemoryManagerPtr inMemoryManager,
    IClientPtr client)
    : Config_(config)
    , Tablet_(tablet)
    , TabletContext_(tabletContext)
    , HydraManager_(std::move(hydraManager))
    , InMemoryManager_(std::move(inMemoryManager))
    , Client_(std::move(client))
    , Logger(TabletNodeLogger)
{
    YCHECK(Config_);
    YCHECK(Tablet_);

    Logger.AddTag("TabletId: %v, CellId: %v",
        Tablet_->GetId(),
        TabletContext_->GetCellId());
}

TTablet* TStoreManagerBase::GetTablet() const
{
    return Tablet_;
}

bool TStoreManagerBase::HasActiveLocks() const
{
    const auto* activeStore = GetActiveStore();
    if (activeStore->GetLockCount() > 0) {
        return true;
    }

    if (!LockedStores_.empty()) {
        return true;
    }

    return false;
}

bool TStoreManagerBase::HasUnflushedStores() const
{
    for (const auto& pair : Tablet_->StoreIdMap()) {
        const auto& store = pair.second;
        auto state = store->GetStoreState();
        if (state != EStoreState::Persistent) {
            return true;
        }
    }
    return false;
}

void TStoreManagerBase::StartEpoch(TTabletSlotPtr slot)
{
    Tablet_->StartEpoch(slot);

    const auto& config = Tablet_->GetConfig();
    LastRotated_ = TInstant::Now() - RandomDuration(config->DynamicStoreAutoFlushPeriod);

    RotationScheduled_ = false;

    // This schedules preload of in-memory tablets.
    UpdateInMemoryMode();
}

void TStoreManagerBase::StopEpoch()
{
    Tablet_->StopEpoch();

    for (const auto& pair : Tablet_->StoreIdMap()) {
        const auto& store = pair.second;
        if (store->IsDynamic()) {
            store->AsDynamic()->SetFlushState(EStoreFlushState::None);
        }
        if (store->IsChunk()) {
            store->AsChunk()->SetCompactionState(EStoreCompactionState::None);
        }
    }
}

bool TStoreManagerBase::IsRotationScheduled() const
{
    return RotationScheduled_;
}

void TStoreManagerBase::ScheduleRotation()
{
    if (RotationScheduled_)
        return;

    RotationScheduled_ = true;

    LOG_INFO("Tablet store rotation scheduled");
}

void TStoreManagerBase::AddStore(IStorePtr store, bool onMount)
{
    Tablet_->AddStore(store);

    if (InMemoryManager_ &&
        store->IsChunk() &&
        Tablet_->GetConfig()->InMemoryMode != EInMemoryMode::None)
    {
        auto chunkStore = store->AsChunk();
        if (!onMount) {
            auto chunkData = InMemoryManager_->EvictInterceptedChunkData(chunkStore->GetId());
            if (!TryPreloadStoreFromInterceptedData(chunkStore, chunkData)) {
                ScheduleStorePreload(chunkStore);
            }
        }
    }
}

void TStoreManagerBase::RemoveStore(IStorePtr store)
{
    YASSERT(store->GetStoreState() != EStoreState::ActiveDynamic);

    store->SetStoreState(EStoreState::Removed);
    Tablet_->RemoveStore(store);
}

void TStoreManagerBase::BackoffStoreRemoval(IStorePtr store)
{
    NConcurrency::TDelayedExecutor::Submit(
        BIND([=] () {
            switch (store->GetType()) {
                case EStoreType::SortedDynamic:
                case EStoreType::OrderedDynamic: {
                    auto dynamicStore = store->AsDynamic();
                    if (dynamicStore->GetFlushState() == EStoreFlushState::Complete) {
                        dynamicStore->SetFlushState(EStoreFlushState::None);
                    }
                    break;
                }
                case EStoreType::SortedChunk:
                case EStoreType::OrderedChunk: {
                    auto chunkStore = store->AsChunk();
                    if (chunkStore->GetCompactionState() == EStoreCompactionState::Complete) {
                        chunkStore->SetCompactionState(EStoreCompactionState::None);
                    }
                    break;
                }
            }
        }).Via(Tablet_->GetEpochAutomatonInvoker()),
        Config_->ErrorBackoffTime);
}

bool TStoreManagerBase::IsStoreFlushable(IStorePtr store) const
{
    if (store->GetStoreState() != EStoreState::PassiveDynamic) {
        return false;
    }

    auto dynamicStore = store->AsDynamic();
    if (dynamicStore->GetFlushState() != EStoreFlushState::None) {
        return false;
    }

    return true;
}

TStoreFlushCallback TStoreManagerBase::BeginStoreFlush(
    IDynamicStorePtr store,
    TTabletSnapshotPtr tabletSnapshot)
{
    YCHECK(store->GetFlushState() == EStoreFlushState::None);
    store->SetFlushState(EStoreFlushState::Running);
    return MakeStoreFlushCallback(store, tabletSnapshot);
}

void TStoreManagerBase::EndStoreFlush(IDynamicStorePtr store)
{
    YCHECK(store->GetFlushState() == EStoreFlushState::Running);
    store->SetFlushState(EStoreFlushState::Complete);
}

void TStoreManagerBase::BackoffStoreFlush(IDynamicStorePtr store)
{
    YCHECK(store->GetFlushState() == EStoreFlushState::Running);
    store->SetFlushState(EStoreFlushState::Failed);
    NConcurrency::TDelayedExecutor::Submit(
        BIND([store] () {
            YCHECK(store->GetFlushState() == EStoreFlushState::Failed);
            store->SetFlushState(EStoreFlushState::None);
        }).Via(Tablet_->GetEpochAutomatonInvoker()),
        Config_->ErrorBackoffTime);
}

void TStoreManagerBase::BeginStoreCompaction(IChunkStorePtr store)
{
    YCHECK(store->GetCompactionState() == EStoreCompactionState::None);
    store->SetCompactionState(EStoreCompactionState::Running);
}

void TStoreManagerBase::EndStoreCompaction(IChunkStorePtr store)
{
    YCHECK(store->GetCompactionState() == EStoreCompactionState::Running);
    store->SetCompactionState(EStoreCompactionState::Complete);
}

void TStoreManagerBase::BackoffStoreCompaction(IChunkStorePtr store)
{
    YCHECK(store->GetCompactionState() == EStoreCompactionState::Running);
    store->SetCompactionState(EStoreCompactionState::Failed);
    NConcurrency::TDelayedExecutor::Submit(
        BIND([store] () {
            YCHECK(store->GetCompactionState() == EStoreCompactionState::Failed);
            store->SetCompactionState(EStoreCompactionState::None);
        }).Via(Tablet_->GetEpochAutomatonInvoker()),
        Config_->ErrorBackoffTime);
}

void TStoreManagerBase::ScheduleStorePreload(IChunkStorePtr store)
{
    auto state = store->GetPreloadState();
    YCHECK(state != EStorePreloadState::Disabled);

    if (state != EStorePreloadState::None && state != EStorePreloadState::Failed) {
        return;
    }

    Tablet_->PreloadStoreIds().push_back(store->GetId());
    store->SetPreloadState(EStorePreloadState::Scheduled);

    LOG_INFO("Scheduled preload of in-memory store (StoreId: %v, Mode: %v)",
        store->GetId(),
        Tablet_->GetConfig()->InMemoryMode);
}

bool TStoreManagerBase::TryPreloadStoreFromInterceptedData(
    IChunkStorePtr store,
    TInMemoryChunkDataPtr chunkData)
{
    YCHECK(store);

    if (!chunkData) {
        LOG_WARNING("Intercepted chunk data for in-memory store is missing (StoreId: %v)",
            store->GetId());
        return false;
    }

    auto state = store->GetPreloadState();
    YCHECK(state == EStorePreloadState::None);

    auto mode = Tablet_->GetConfig()->InMemoryMode;
    YCHECK(mode != EInMemoryMode::None);

    if (mode != chunkData->InMemoryMode) {
        LOG_WARNING("Intercepted chunk data for in-memory store has invalid mode (StoreId: %v, ExpectedMode: %v, ActualMode: %v)",
            store->GetId(),
            mode,
            chunkData->InMemoryMode);
        return false;
    }

    store->Preload(chunkData);
    store->SetPreloadState(EStorePreloadState::Complete);

    LOG_INFO("In-memory store preloaded from intercepted chunk data (StoreId: %v, Mode: %v)",
        store->GetId(),
        mode);

    return true;
}

IChunkStorePtr TStoreManagerBase::PeekStoreForPreload()
{
    while (!Tablet_->PreloadStoreIds().empty()) {
        auto id = Tablet_->PreloadStoreIds().front();
        auto store = Tablet_->FindStore(id);
        if (store) {
            auto chunkStore = store->AsChunk();
            if (chunkStore->GetPreloadState() == EStorePreloadState::Scheduled) {
                return chunkStore;
            }
        }
        Tablet_->PreloadStoreIds().pop_front();
    }
    return nullptr;
}

void TStoreManagerBase::BeginStorePreload(IChunkStorePtr store, TFuture<void> future)
{
    YCHECK(store->GetId() == Tablet_->PreloadStoreIds().front());
    Tablet_->PreloadStoreIds().pop_front();
    store->SetPreloadState(EStorePreloadState::Running);
    store->SetPreloadFuture(future);
}

void TStoreManagerBase::EndStorePreload(IChunkStorePtr store)
{
    store->SetPreloadState(EStorePreloadState::Complete);
    store->SetPreloadFuture(TFuture<void>());
}

void TStoreManagerBase::BackoffStorePreload(IChunkStorePtr store)
{
    if (store->GetPreloadState() != EStorePreloadState::Running) {
        return;
    }

    store->SetPreloadState(EStorePreloadState::Failed);
    store->SetPreloadFuture(TFuture<void>());
    NConcurrency::TDelayedExecutor::Submit(
        BIND([=] () {
            if (store->GetPreloadState() == EStorePreloadState::Failed) {
                ScheduleStorePreload(store);
            }
        }).Via(Tablet_->GetEpochAutomatonInvoker()),
        Config_->ErrorBackoffTime);
}

void TStoreManagerBase::Mount(const std::vector<TAddStoreDescriptor>& storeDescriptors)
{
    for (const auto& descriptor : storeDescriptors) {
        auto type = EStoreType(descriptor.store_type());
        auto storeId = FromProto<TChunkId>(descriptor.store_id());
        YCHECK(descriptor.has_chunk_meta());
        YCHECK(!descriptor.has_backing_store_id());
        auto store = TabletContext_->CreateStore(
            Tablet_,
            type,
            storeId,
            &descriptor);
        AddStore(store->AsChunk(), true);
    }

    // NB: Active store must be created _after_ chunk stores to make sure it receives
    // the right starting row index (for ordered tablets only).
    CreateActiveStore();

    Tablet_->SetState(ETabletState::Mounted);
}

void TStoreManagerBase::Remount(TTableMountConfigPtr mountConfig, TTabletWriterOptionsPtr writerOptions)
{
    Tablet_->SetConfig(mountConfig);
    Tablet_->SetWriterOptions(writerOptions);

    UpdateInMemoryMode();
}

void TStoreManagerBase::Rotate(bool createNewStore)
{
    RotationScheduled_ = false;
    LastRotated_ = TInstant::Now();

    auto* activeStore = GetActiveStore();
    YCHECK(activeStore);
    activeStore->SetStoreState(EStoreState::PassiveDynamic);

    if (activeStore->GetLockCount() > 0) {
        LOG_INFO_UNLESS(IsRecovery(), "Active store is locked and will be kept (StoreId: %v, LockCount: %v)",
            activeStore->GetId(),
            activeStore->GetLockCount());
        YCHECK(LockedStores_.insert(IStorePtr(activeStore)).second);
    } else {
        LOG_INFO_UNLESS(IsRecovery(), "Active store is not locked and will be dropped (StoreId: %v)",
            activeStore->GetId(),
            activeStore->GetLockCount());
    }

    OnActiveStoreRotated();

    if (createNewStore) {
        CreateActiveStore();
    } else {
        ResetActiveStore();
        Tablet_->SetActiveStore(nullptr);
    }

    LOG_INFO_UNLESS(IsRecovery(), "Tablet stores rotated");
}

bool TStoreManagerBase::IsStoreLocked(IStorePtr store) const
{
    return LockedStores_.find(store) != LockedStores_.end();
}

std::vector<IStorePtr> TStoreManagerBase::GetLockedStores() const
{
    return std::vector<IStorePtr>(LockedStores_.begin(), LockedStores_.end());
}

bool TStoreManagerBase::IsOverflowRotationNeeded() const
{
    if (!IsRotationPossible()) {
        return false;
    }

    const auto* activeStore = GetActiveStore();
    const auto& config = Tablet_->GetConfig();
    return
        activeStore->GetRowCount() >= config->MaxDynamicStoreRowCount ||
        activeStore->GetValueCount() >= config->MaxDynamicStoreValueCount ||
        activeStore->GetPoolCapacity() >= config->MaxDynamicStorePoolSize;
}

bool TStoreManagerBase::IsPeriodicRotationNeeded() const
{
    if (!IsRotationPossible()) {
        return false;
    }

    const auto* activeStore = GetActiveStore();
    const auto& config = Tablet_->GetConfig();
    return
        TInstant::Now() > LastRotated_ + config->DynamicStoreAutoFlushPeriod &&
        activeStore->GetRowCount() > 0;
}

bool TStoreManagerBase::IsRotationPossible() const
{
    if (IsRotationScheduled()) {
        return false;
    }

    auto* activeStore = GetActiveStore();
    if (!activeStore) {
        return false;
    }

    // NB: For ordered tablets, we must never attempt to rotate an empty store
    // to avoid collisions of starting row indexes. This check, however, makes
    // sense for sorted tablets as well.
    if (activeStore->GetRowCount() == 0) {
        return false;
    }

    return true;
}

bool TStoreManagerBase::IsForcedRotationPossible() const
{
    if (!IsRotationPossible()) {
        return false;
    }

    // Check for "almost" initial size.
    const auto* activeStore = GetActiveStore();
    if (activeStore->GetPoolCapacity() <= 2 * Config_->PoolChunkSize) {
        return false;
    }

    return true;
}

ISortedStoreManagerPtr TStoreManagerBase::AsSorted()
{
    YUNREACHABLE();
}

IOrderedStoreManagerPtr TStoreManagerBase::AsOrdered()
{
    YUNREACHABLE();
}

void TStoreManagerBase::CheckForUnlockedStore(IDynamicStore* store)
{
    if (store == GetActiveStore() || store->GetLockCount() > 0) {
        return;
    }

    LOG_INFO_UNLESS(IsRecovery(), "Store unlocked and will be dropped (StoreId: %v)",
        store->GetId());
    YCHECK(LockedStores_.erase(store) == 1);
}

void TStoreManagerBase::UpdateInMemoryMode()
{
    auto mode = Tablet_->GetConfig()->InMemoryMode;

    for (const auto& storeId : Tablet_->PreloadStoreIds()) {
        auto chunkStore = Tablet_->FindStore(storeId)->AsChunk();
        YCHECK(chunkStore->GetPreloadState() == EStorePreloadState::Scheduled);
        chunkStore->SetPreloadState(EStorePreloadState::None);
    }
    Tablet_->PreloadStoreIds().clear();

    for (const auto& pair : Tablet_->StoreIdMap()) {
        const auto& store = pair.second;
        if (store->IsChunk()) {
            auto chunkStore = store->AsChunk();
            chunkStore->SetInMemoryMode(mode);
            if (mode != EInMemoryMode::None) {
                ScheduleStorePreload(chunkStore);
            }
        }
    }
}

bool TStoreManagerBase::IsRecovery() const
{
    // NB: HydraManager is null in tests.
    return HydraManager_ ? HydraManager_->IsRecovery() : false;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT

