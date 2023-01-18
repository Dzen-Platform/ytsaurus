#include "tablet_base.h"

#include "tablet_action.h"
#include "tablet_cell.h"
#include "tablet_owner_base.h"

#include <yt/yt/server/master/chunk_server/chunk_list.h>
#include <yt/yt/server/master/chunk_server/chunk_manager.h>
#include <yt/yt/server/master/chunk_server/chunk_tree_traverser.h>

#include <yt/yt/server/lib/tablet_server/proto/tablet_manager.pb.h>

namespace NYT::NTabletServer {

using namespace NChunkClient;
using namespace NChunkServer;
using namespace NCellMaster;
using namespace NCypressClient;
using namespace NTabletClient;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

void TTabletBase::Save(TSaveContext& context) const
{
    TObject::Save(context);

    using NYT::Save;
    Save(context, Index_);
    Save(context, InMemoryMode_);
    Save(context, Cell_);
    Save(context, MountRevision_);
    Save(context, SettingsRevision_);
    Save(context, WasForcefullyUnmounted_);
    Save(context, Action_);
    Save(context, StoresUpdatePreparedTransaction_);
    Save(context, Owner_);
    Save(context, State_);
    Save(context, ExpectedState_);
    Save(context, TabletErrorCount_);
}

void TTabletBase::Load(TLoadContext& context)
{
    TObject::Load(context);

    // COMPAT(gritukan)
    if (context.GetVersion() < EMasterReign::TabletBase) {
        return;
    }

    using NYT::Load;
    Load(context, Index_);
    Load(context, InMemoryMode_);
    Load(context, Cell_);
    Load(context, MountRevision_);
    // COMPAT(ifsmirnov)
    if (context.GetVersion() >= EMasterReign::RemountNeededNotification) {
        Load(context, SettingsRevision_);
    }
    Load(context, WasForcefullyUnmounted_);
    Load(context, Action_);
    Load(context, StoresUpdatePreparedTransaction_);
    Load(context, Owner_);
    Load(context, State_);
    Load(context, ExpectedState_);
    Load(context, TabletErrorCount_);
}

ETabletState TTabletBase::GetState() const
{
    return State_;
}

void TTabletBase::SetState(ETabletState state)
{
    if (Owner_) {
        auto* owner = Owner_->GetTrunkNode();
        YT_VERIFY(owner->TabletCountByState()[State_] > 0);
        --owner->MutableTabletCountByState()[State_];
        ++owner->MutableTabletCountByState()[state];
    }

    if (!Action_) {
        SetExpectedState(state);
    }

    State_ = state;
}

void TTabletBase::SetStateCompat(ETabletState state)
{
    State_ = state;
}

ETabletState TTabletBase::GetExpectedState() const
{
    return ExpectedState_;
}

void TTabletBase::SetExpectedState(ETabletState state)
{
    if (Owner_) {
        auto* owner = Owner_->GetTrunkNode();
        YT_VERIFY(owner->TabletCountByExpectedState()[ExpectedState_] > 0);
        --owner->MutableTabletCountByExpectedState()[ExpectedState_];
        ++owner->MutableTabletCountByExpectedState()[state];
    }
    ExpectedState_ = state;
}

void TTabletBase::SetExpectedStateCompat(ETabletState state)
{
    ExpectedState_ = state;
}

TTabletOwnerBase* TTabletBase::GetOwner() const
{
    return Owner_;
}

void TTabletBase::SetOwner(TTabletOwnerBase* owner)
{
    if (Owner_) {
        YT_VERIFY(Owner_->GetTrunkNode()->TabletCountByState()[State_] > 0);
        YT_VERIFY(Owner_->GetTrunkNode()->TabletCountByExpectedState()[ExpectedState_] > 0);
        --Owner_->GetTrunkNode()->MutableTabletCountByState()[State_];
        --Owner_->GetTrunkNode()->MutableTabletCountByExpectedState()[ExpectedState_];

        int restTabletErrorCount = Owner_->GetTabletErrorCount() - GetTabletErrorCount();
        YT_ASSERT(restTabletErrorCount >= 0);
        Owner_->SetTabletErrorCount(restTabletErrorCount);
    }
    if (owner) {
        YT_VERIFY(owner->IsTrunk());
        ++owner->MutableTabletCountByState()[State_];
        ++owner->MutableTabletCountByExpectedState()[ExpectedState_];

        owner->SetTabletErrorCount(owner->GetTabletErrorCount() + GetTabletErrorCount());
    }
    Owner_ = owner;
}

// COMPAT(gritukan)
void TTabletBase::SetOwnerCompat(TTabletOwnerBase* owner)
{
    Owner_ = owner;
}

void TTabletBase::CopyFrom(const TTabletBase& other)
{
    YT_VERIFY(State_ == ETabletState::Unmounted);
    YT_VERIFY(!Cell_);

    Index_ = other.Index_;
    MountRevision_ = other.MountRevision_;
    InMemoryMode_ = other.InMemoryMode_;
}

void TTabletBase::ValidateMountRevision(NHydra::TRevision mountRevision)
{
    if (MountRevision_ != mountRevision) {
        THROW_ERROR_EXCEPTION(
            NRpc::EErrorCode::Unavailable,
            "Invalid mount revision of tablet %v: expected %x, received %x",
            Id_,
            MountRevision_,
            mountRevision);
    }
}

bool TTabletBase::IsActive() const
{
    return
        State_ == ETabletState::Mounting ||
        State_ == ETabletState::FrozenMounting ||
        State_ == ETabletState::Mounted ||
        State_ == ETabletState::Freezing ||
        State_ == ETabletState::Frozen ||
        State_ == ETabletState::Unfreezing;
}

NChunkServer::TChunkList* TTabletBase::GetChunkList()
{
    return GetChunkList(EChunkListContentType::Main);
}

const NChunkServer::TChunkList* TTabletBase::GetChunkList() const
{
    return GetChunkList(EChunkListContentType::Main);
}

NChunkServer::TChunkList* TTabletBase::GetHunkChunkList()
{
    return GetChunkList(EChunkListContentType::Hunk);
}

const NChunkServer::TChunkList* TTabletBase::GetHunkChunkList() const
{
    return GetChunkList(EChunkListContentType::Hunk);
}

TChunkList* TTabletBase::GetChunkList(EChunkListContentType type)
{
    if (auto* rootChunkList = Owner_->GetTrunkNode()->GetChunkList(type)) {
        return rootChunkList->Children()[Index_]->AsChunkList();
    } else {
        return nullptr;
    }
}

const TChunkList* TTabletBase::GetChunkList(EChunkListContentType type) const
{
    return const_cast<TTabletBase*>(this)->GetChunkList(type);
}

i64 TTabletBase::GetTabletStaticMemorySize(EInMemoryMode mode) const
{
    // TODO(savrus) consider lookup hash table.

    const auto& statistics = GetChunkList()->Statistics();
    switch (mode) {
        case EInMemoryMode::Compressed:
            return statistics.CompressedDataSize;
        case EInMemoryMode::Uncompressed:
            return statistics.UncompressedDataSize;
        case EInMemoryMode::None:
            return 0;
        default:
            YT_ABORT();
    }
}

i64 TTabletBase::GetTabletStaticMemorySize() const
{
    return GetTabletStaticMemorySize(GetInMemoryMode());
}

i64 TTabletBase::GetTabletMasterMemoryUsage() const
{
    return sizeof(TTabletBase);
}

void TTabletBase::ValidateMount(bool freeze)
{
    if (State_ != ETabletState::Unmounted && (freeze
        ? State_ != ETabletState::Frozen &&
            State_ != ETabletState::Freezing &&
            State_ != ETabletState::FrozenMounting
        : State_ != ETabletState::Mounted &&
            State_ != ETabletState::Mounting &&
            State_ != ETabletState::Unfreezing))
    {
        THROW_ERROR_EXCEPTION("Cannot mount tablet %v in %Qlv state",
            Id_,
            State_);
    }

    std::vector<TChunkTree*> stores;
    for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
        if (auto* chunkList = GetChunkList(contentType)) {
            EnumerateStoresInChunkTree(chunkList, &stores);
        }
    }

    THashSet<TObjectId> storeSet;
    storeSet.reserve(stores.size());
    for (auto* store : stores) {
        if (!storeSet.insert(store->GetId()).second) {
            THROW_ERROR_EXCEPTION("Cannot mount %v: tablet %v contains duplicate store %v of type %Qlv",
                GetOwner()->GetType(),
                Id_,
                store->GetId(),
                store->GetType());
        }
    }
}

void TTabletBase::ValidateUnmount()
{
    if (State_ != ETabletState::Mounted &&
        State_ != ETabletState::Frozen &&
        State_ != ETabletState::Unmounted &&
        State_ != ETabletState::Unmounting)
    {
        THROW_ERROR_EXCEPTION("Cannot unmount tablet %v in %Qlv state",
            Id_,
            State_);
    }
}

void TTabletBase::ValidateFreeze() const
{
    if (State_ != ETabletState::Mounted &&
        State_ != ETabletState::FrozenMounting &&
        State_ != ETabletState::Freezing &&
        State_ != ETabletState::Frozen)
    {
        THROW_ERROR_EXCEPTION("Cannot freeze tablet %v in %Qlv state",
            Id_,
            State_);
    }
}

void TTabletBase::ValidateUnfreeze() const
{
    if (State_ != ETabletState::Mounted &&
        State_ != ETabletState::Frozen &&
        State_ != ETabletState::Unfreezing)
    {
        THROW_ERROR_EXCEPTION("Cannot unfreeze tablet %v in %Qlv state",
            Id_,
            State_);
    }
}

void TTabletBase::ValidateReshard() const
{
    if (State_ != ETabletState::Unmounted) {
        THROW_ERROR_EXCEPTION("Cannot reshard table since tablet %v is not unmounted",
            Id_);
    }
}

void TTabletBase::ValidateReshardRemove() const
{ }

int TTabletBase::GetTabletErrorCount() const
{
    return TabletErrorCount_;
}

void TTabletBase::SetTabletErrorCount(int tabletErrorCount)
{
    if (Owner_) {
        int restTabletErrorCount = Owner_->GetTabletErrorCount() - GetTabletErrorCount();
        YT_ASSERT(restTabletErrorCount >= 0);
        Owner_->SetTabletErrorCount(restTabletErrorCount + tabletErrorCount);
    }

    TabletErrorCount_ = tabletErrorCount;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
