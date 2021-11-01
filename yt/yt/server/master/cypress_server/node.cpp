#include "node.h"
#include "shard.h"

#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/security_server/account.h>

#include <yt/yt/server/master/transaction_server/transaction.h>

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/server/lib/hydra/hydra_context.h>

namespace NYT::NCypressServer {

using namespace NObjectClient;
using namespace NObjectServer;
using namespace NSecurityServer;
using namespace NTransactionServer;
using namespace NCellMaster;

using NTabletServer::TTabletResources;

////////////////////////////////////////////////////////////////////////////////

void TNullVersionedBuiltinAttribute::Persist(const NCellMaster::TPersistenceContext& /*context*/)
{ }

void TNullVersionedBuiltinAttribute::Persist(const NCypressServer::TCopyPersistenceContext& /*context*/)
{ }

void TTombstonedVersionedBuiltinAttribute::Persist(const NCellMaster::TPersistenceContext& /*context*/)
{ }

void TTombstonedVersionedBuiltinAttribute::Persist(const NCypressServer::TCopyPersistenceContext& /*context*/)
{ }

////////////////////////////////////////////////////////////////////////////////

TCypressNode::TCypressNode(TVersionedNodeId id)
    : TObject(id.ObjectId)
    , Acd_(this)
    , TransactionId_(id.TransactionId)
{
    if (TransactionId_) {
        Flags_.Trunk = false;
    }
}

TCypressNode::~TCypressNode() = default;

TCypressNode* TCypressNode::GetParent() const
{
    return Parent_;
}

void TCypressNode::SetParent(TCypressNode* parent)
{
    if (Parent_ == parent)
        return;

    // Drop old parent.
    if (Parent_) {
        YT_VERIFY(Parent_->ImmediateDescendants().erase(this) == 1);
    }

    // Set new parent.
    Parent_ = parent;
    if (Parent_) {
        YT_VERIFY(Parent_->IsTrunk());
        YT_VERIFY(Parent_->ImmediateDescendants().insert(this).second);
    }
}

void TCypressNode::ResetParent()
{
    Parent_ = nullptr;
}

TCypressNode* TCypressNode::GetOriginator() const
{
    return Originator_;
}

void TCypressNode::SetOriginator(TCypressNode* originator)
{
    Originator_ = originator;
}

const TCypressNodeLockingState& TCypressNode::LockingState() const
{
    return LockingState_ ? *LockingState_ : TCypressNodeLockingState::Empty;
}

TCypressNodeLockingState* TCypressNode::MutableLockingState()
{
    if (!LockingState_) {
        LockingState_ = std::make_unique<TCypressNodeLockingState>();
    }
    return LockingState_.get();
}

bool TCypressNode::HasLockingState() const
{
    return LockingState_.operator bool();
}

void TCypressNode::ResetLockingState()
{
    LockingState_.reset();
}

void TCypressNode::ResetLockingStateIfEmpty()
{
    if (LockingState_ && LockingState_->IsEmpty()) {
        LockingState_.reset();
    }
}

TVersionedNodeId TCypressNode::GetVersionedId() const
{
    return TVersionedNodeId(Id_, TransactionId_);
}

bool TCypressNode::IsExternal() const
{
    return ExternalCellTag_ >= MinValidCellTag && ExternalCellTag_ <= MaxValidCellTag;
}

TClusterResources TCypressNode::GetDeltaResourceUsage() const
{
    YT_VERIFY(!IsExternal());

    return TClusterResources()
        .SetNodeCount(1);
}

TClusterResources TCypressNode::GetTotalResourceUsage() const
{
    return TClusterResources()
        .SetNodeCount(1);
}

TDetailedMasterMemory TCypressNode::GetDetailedMasterMemoryUsage() const
{
    TDetailedMasterMemory result;
    if (Attributes_) {
        result[EMasterMemoryType::Attributes] += Attributes_->GetMasterMemoryUsage();
    }
    return result;
}

TTabletResources TCypressNode::GetTabletResourceUsage() const
{
    return {};
}

NHydra::TRevision TCypressNode::GetRevision() const
{
    return Max(AttributeRevision_, ContentRevision_);
}

NHydra::TRevision TCypressNode::GetNativeContentRevision() const
{
    YT_VERIFY(IsForeign());
    return NativeContentRevision_;
}

void TCypressNode::SetNativeContentRevision(NHydra::TRevision revision)
{
    YT_VERIFY(IsForeign());
    NativeContentRevision_ = revision;
}

bool TCypressNode::IsBeingCreated() const
{
    return GetRevision() == NHydra::GetCurrentHydraContext()->GetVersion().ToRevision();
}

bool TCypressNode::CanCacheResolve() const
{
    if (!TrunkNode_->LockingState().TransactionToExclusiveLocks.empty()) {
        return false;
    }
    if (GetNodeType() != NYTree::ENodeType::Map &&
        GetType() != EObjectType::Link &&
        GetType() != EObjectType::PortalEntrance)
    {
        return false;
    }
    return true;
}

void TCypressNode::Save(TSaveContext& context) const
{
    TObject::Save(context);

    using NYT::Save;
    Save(context, ExternalCellTag_);
    if (LockingState_) {
        Save(context, true);
        Save(context, *LockingState_);
    } else {
        Save(context, false);
    }
    TRawNonversionedObjectPtrSerializer::Save(context, Parent_);
    Save(context, LockMode_);
    Save(context, ExpirationTime_);
    Save(context, ExpirationTimeout_);
    Save(context, CreationTime_);
    Save(context, ModificationTime_);
    Save(context, AttributeRevision_);
    Save(context, ContentRevision_);
    Save(context, NativeContentRevision_);
    Save(context, Account_);
    Save(context, Acd_);
    Save(context, Opaque_);
    Save(context, AccessTime_);
    Save(context, AccessCounter_);
    Save(context, Shard_);
    Save(context, Annotation_);
}

void TCypressNode::Load(TLoadContext& context)
{
    TObject::Load(context);

    using NYT::Load;
    Load(context, ExternalCellTag_);
    if (Load<bool>(context)) {
        LockingState_ = std::make_unique<TCypressNodeLockingState>();
        Load(context, *LockingState_);
    }
    TRawNonversionedObjectPtrSerializer::Load(context, Parent_);
    Load(context, LockMode_);
    Load(context, ExpirationTime_);
    // COMPAT(shakurov)
    if (context.GetVersion() >= EMasterReign::YT_13126_ExpirationTimeout) {
        Load(context, ExpirationTimeout_);
    }
    Load(context, CreationTime_);
    Load(context, ModificationTime_);
    Load(context, AttributeRevision_);
    Load(context, ContentRevision_);
    // COMPAT(shakurov)
    if (context.GetVersion() >= EMasterReign::NativeContentRevision) {
        Load(context, NativeContentRevision_);
    }
    Load(context, Account_);
    Load(context, Acd_);
    Load(context, Opaque_);
    Load(context, AccessTime_);
    Load(context, AccessCounter_);
    Load(context, Shard_);
    // COMPAT(shakurov)
    if (context.GetVersion() < EMasterReign::CorrectMergeBranchSemanticsForAttributes) {
        auto annotation = Load<std::optional<TString>>(context);
        if (annotation) {
            Annotation_.Set(*annotation);
        } else {
            Annotation_.Reset();
        }
    } else {
        Load(context, Annotation_);
    }
}

void TCypressNode::SaveEctoplasm(TStreamSaveContext& context) const
{
    TObject::SaveEctoplasm(context);

    using NYT::Save;
    Save(context, reinterpret_cast<uintptr_t>(TrunkNode_));
    Save(context, TransactionId_);
}

void TCypressNode::LoadEctoplasm(TStreamLoadContext& context)
{
    TObject::LoadEctoplasm(context);

    using NYT::Load;
    Load(context, reinterpret_cast<uintptr_t&>(TrunkNode_));
    Load(context, TransactionId_);
}

////////////////////////////////////////////////////////////////////////////////

TVersionedObjectId GetObjectId(const TCypressNode* object)
{
    return object ? object->GetVersionedId() : TVersionedObjectId(NullObjectId, NullTransactionId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer

