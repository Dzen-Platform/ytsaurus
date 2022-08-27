#include "hunk_tablet.h"

#include "private.h"
#include "hunk_store.h"
#include "hunk_tablet_manager.h"
#include "serialize.h"

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NTabletNode {

using namespace NHydra;
using namespace NJournalClient;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

THunkTablet::THunkTablet(
    IHunkTabletHostPtr host,
    TTabletId tabletId)
    : TObjectBase(tabletId)
    , Host_(std::move(host))
    , Logger(TabletNodeLogger.WithTag("TabletId: %v", tabletId))
{ }

void THunkTablet::Save(TSaveContext& context) const
{
    using NYT::Save;

    Save(context, State_);
    Save(context, MountRevision_);
    Save(context, *MountConfig_);
    Save(context, *StoreWriterConfig_);
    Save(context, *StoreWriterOptions_);

    {
        TSizeSerializer::Save(context, IdToStore_.size());
        for (auto it : GetSortedIterators(IdToStore_)) {
            const auto& [storeId, store] = *it;
            Save(context, storeId);
            store->Save(context);
        }
    }
}

void THunkTablet::Load(TLoadContext& context)
{
    using NYT::Load;

    Load(context, State_);
    Load(context, MountRevision_);
    Load(context, *MountConfig_);
    Load(context, *StoreWriterConfig_);
    Load(context, *StoreWriterOptions_);

    {
        auto storeCount = TSizeSerializer::Load(context);
        for (int index = 0; index < static_cast<int>(storeCount); ++index) {
            auto storeId = Load<TStoreId>(context);
            auto store = New<THunkStore>(storeId, this);
            store->Load(context);
        }
    }
}

TFuture<std::vector<TJournalHunkDescriptor>> THunkTablet::WriteHunks(std::vector<TSharedRef> payloads)
{
    auto automatonInvoker = GetCurrentInvoker();

    auto doWriteHunks = [=] (std::vector<TSharedRef> payloads, THunkStorePtr store) {
        auto tabletId = GetId();

        store->SetLastWriteTime(TInstant::Now());

        auto future = store->WriteHunks(std::move(payloads));
        future.Subscribe(BIND([=] (const TErrorOr<std::vector<TJournalHunkDescriptor>>& descriptorsOrError) {
            store->SetLastWriteTime(TInstant::Now());

            if (!descriptorsOrError.IsOK() && ActiveStore_ == store) {
                YT_LOG_DEBUG(descriptorsOrError, "Failed to write hunks, rotating active store "
                    "(StoreId: %v)",
                    store->GetId());

                RotateActiveStore();

                Host_->ScheduleScanTablet(tabletId);
            }
        }).Via(automatonInvoker));

        return future;
    };

    if (ActiveStore_) {
        // Fast path.
        return doWriteHunks(std::move(payloads), ActiveStore_);
    }

    // Slow path.
    auto future = ActiveStorePromise_.ToFuture();
    return future.Apply(BIND(doWriteHunks, payloads).AsyncVia(automatonInvoker));
}

void THunkTablet::Reconfigure(const THunkStorageSettings& settings)
{
    MountConfig_ = settings.MountConfig;
    StoreWriterConfig_ = settings.StoreWriterConfig;
    StoreWriterOptions_ = settings.StoreWriterOptions;
}

THunkStorePtr THunkTablet::FindStore(TStoreId storeId)
{
    auto storeIt = IdToStore_.find(storeId);
    if (storeIt == IdToStore_.end()) {
        return nullptr;
    } else {
        return storeIt->second;
    }
}

THunkStorePtr THunkTablet::GetStore(TStoreId storeId)
{
    return GetOrCrash(IdToStore_, storeId);
}

THunkStorePtr THunkTablet::GetStoreOrThrow(TStoreId storeId)
{
    if (auto store = FindStore(storeId)) {
        return store;
    } else {
        THROW_ERROR_EXCEPTION("No such store %v", storeId)
            << TErrorAttribute("store_id", storeId);
    }
}

void THunkTablet::AddStore(THunkStorePtr store)
{
    YT_VERIFY(HasHydraContext());

    EmplaceOrCrash(IdToStore_, store->GetId(), store);

    if (store->GetState() == EHunkStoreState::Allocated) {
        InsertOrCrash(AllocatedStores_, store);
    } else if (store->GetState() == EHunkStoreState::Passive) {
        InsertOrCrash(PassiveStores_, store);
    }

    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
        "Store added (StoreId: %v, StoreState: %v)",
        store->GetId(),
        store->GetState());
}

void THunkTablet::RemoveStore(const THunkStorePtr& store)
{
    YT_VERIFY(HasHydraContext());

    YT_VERIFY(store->GetState() == EHunkStoreState::Passive);
    YT_VERIFY(store->GetMarkedSealable());

    // NB: May be missing during recovery.
    PassiveStores_.erase(store);

    auto storeId = store->GetId();
    auto storeState = store->GetState();
    EraseOrCrash(IdToStore_, storeId);

    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
        "Store removed (StoreId: %v, StoreState: %v)",
        storeId,
        storeState);
}

bool THunkTablet::IsReadyToUnmount(bool force) const
{
    // Force unmount is always possible.
    if (force) {
        return true;
    }

    // Cannot unmount tablet when there are alive stores.
    if (!IdToStore_.empty()) {
        return false;
    }

    // Tablet is locked by some transaction.
    if (LockTransactionId_) {
        return false;
    }

    // Seems good.
    return true;
}

bool THunkTablet::IsFullyUnlocked(bool forceUnmount) const
{
    // Persistently locked.
    if (!IsReadyToUnmount(forceUnmount)) {
        return false;
    }

    // Tablet scanner is scanning tablet now.
    if (LockedByScan_) {
        return false;
    }

    // Seems good.
    return true;
}

void THunkTablet::OnUnmount()
{
    MakeAllStoresPassive();

    auto error = TError(
        NTabletClient::EErrorCode::TabletNotMounted,
        "Tablet %v is unmounted",
        Id_);
    ActiveStorePromise_.TrySet(error);
    ActiveStorePromise_ = MakePromise<THunkStorePtr>(error);
}

void THunkTablet::OnStopLeading()
{
    MakeAllStoresPassive();

    auto error = TError(
        NRpc::EErrorCode::Unavailable,
        "Tablet cell stopped leading",
        Id_);
    ActiveStorePromise_.TrySet(error);
    ActiveStorePromise_ = MakePromise<THunkStorePtr>(error);
}

void THunkTablet::RotateActiveStore()
{
    auto oldActiveStoreId = NullStoreId;
    if (ActiveStore_) {
        ActiveStore_->SetState(EHunkStoreState::Passive);
        oldActiveStoreId = ActiveStore_->GetId();
        PassiveStores_.insert(ActiveStore_);
        ActiveStore_.Reset();
        ActiveStorePromise_ = NewPromise<THunkStorePtr>();
    }

    auto newActiveStoreId = NullStoreId;
    if (!AllocatedStores_.empty()) {
        auto newActiveStore = *AllocatedStores_.begin();
        EraseOrCrash(AllocatedStores_, newActiveStore);
        ActiveStore_ = newActiveStore;
        ActiveStore_->SetState(EHunkStoreState::Active);

        // NB: May be already set in case of errors.
        if (ActiveStorePromise_.IsSet()) {
            ActiveStorePromise_ = NewPromise<THunkStorePtr>();
        }
        ActiveStorePromise_.Set(ActiveStore_);

        newActiveStoreId = newActiveStore->GetId();
    }

    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
        "Active store rotated (ActiveStoreId: %v -> %v)",
        oldActiveStoreId,
        newActiveStoreId);
}

void THunkTablet::LockTransaction(TTransactionId transactionId)
{
    if (LockTransactionId_) {
        THROW_ERROR_EXCEPTION("Tablet %v is already locked by transaction %v",
            Id_,
            LockTransactionId_);
    }

    LockTransactionId_ = transactionId;
}

void THunkTablet::UnlockTransaction(TTransactionId transactionId)
{
    YT_VERIFY(!LockTransactionId_ || LockTransactionId_ == transactionId);
    LockTransactionId_ = NullTransactionId;
}

bool THunkTablet::IsLockedByTransaction() const
{
    return static_cast<bool>(LockTransactionId_);
}

bool THunkTablet::TryLockScan()
{
    return !std::exchange(LockedByScan_, true);
}

void THunkTablet::UnlockScan()
{
    YT_VERIFY(LockedByScan_);
    LockedByScan_ = false;
}

void THunkTablet::ValidateMountRevision(TRevision mountRevision) const
{
    if (mountRevision != MountRevision_) {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::InvalidMountRevision,
            "Invalid mount revision of tablet %v: expected %x, received %x",
            Id_,
            MountRevision_,
            mountRevision);
    }
}

void THunkTablet::ValidateMounted(NHydra::TRevision mountRevision) const
{
    if (State_ != ETabletState::Mounted) {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::TabletNotMounted,
            "Tablet %v is not mounted",
            Id_)
            << TErrorAttribute("state", State_);
    }

    ValidateMountRevision(mountRevision);
}

TFuture<THunkStorePtr> THunkTablet::GetActiveStoreFuture() const
{
    return ActiveStorePromise_.ToFuture();
}

void THunkTablet::BuildOrchidYson(IYsonConsumer* consumer) const
{
    BuildYsonFluently(consumer).BeginMap()
        .Item("id").Value(Id_)
        .Item("state").Value(State_)
        .Item("mount_revision").Value(MountRevision_)
        .Item("stores").DoMapFor(IdToStore_, [&] (auto fluent, auto item) {
            fluent.Item(ToString(item.first)).Do([&] (auto fluent) {
                item.second->BuildOrchidYson(fluent.GetConsumer());
            });
        })
        .DoIf(static_cast<bool>(ActiveStore_), [&] (auto fluent) {
            fluent.Item("active_store_id").Value(ActiveStore_->GetId());
        })
    .EndMap();
}

const NLogging::TLogger& THunkTablet::GetLogger() const
{
    return Logger;
}

bool THunkTablet::IsMutationLoggingEnabled() const
{
    return Host_->IsMutationLoggingEnabled();
}

void THunkTablet::MakeAllStoresPassive()
{
    if (ActiveStore_) {
        ActiveStore_->SetState(EHunkStoreState::Passive);
        InsertOrCrash(PassiveStores_, ActiveStore_);
        ActiveStore_.Reset();
    }

    for (const auto& store : AllocatedStores_) {
        store->SetState(EHunkStoreState::Passive);
        InsertOrCrash(PassiveStores_, store);
    }
    AllocatedStores_.clear();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
