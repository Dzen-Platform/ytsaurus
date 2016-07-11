#include "node.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/security_server/account.h>

#include <yt/server/transaction_server/transaction.h>

namespace NYT {
namespace NCypressServer {

using namespace NObjectClient;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

TCypressNodeBase::TCypressNodeBase(const TVersionedNodeId& id)
    : TObjectBase(id.ObjectId)
    , ExternalCellTag_(NotReplicatedCellTag)
    , AccountingEnabled_(true)
    , LockMode_(ELockMode::None)
    , TrunkNode_(nullptr)
    , Transaction_(nullptr)
    , CreationTime_(0)
    , ModificationTime_(0)
    , AccessTime_(0)
    , AccessCounter_(0)
    , Revision_(0)
    , Account_(nullptr)
    , Acd_(this)
    , Parent_(nullptr)
    , Originator_(nullptr)
    , TransactionId_(id.TransactionId)
{ }

TCypressNodeBase::~TCypressNodeBase() = default;

TCypressNodeBase* TCypressNodeBase::GetParent() const
{
    return Parent_;
}

void TCypressNodeBase::SetParent(TCypressNodeBase* parent)
{
    if (Parent_ == parent)
        return;

    // Drop old parent.
    if (Parent_) {
        YCHECK(Parent_->ImmediateDescendants().erase(this) == 1);
    }

    // Set new parent.
    Parent_ = parent;
    if (Parent_) {
        YCHECK(Parent_->IsTrunk());
        YCHECK(Parent_->ImmediateDescendants().insert(this).second);
    }
}

void TCypressNodeBase::ResetParent()
{
    Parent_ = nullptr;
}

TCypressNodeBase* TCypressNodeBase::GetOriginator() const
{
    return Originator_;
}

void TCypressNodeBase::SetOriginator(TCypressNodeBase* originator)
{
    Originator_ = originator;
}

TVersionedNodeId TCypressNodeBase::GetVersionedId() const
{
    return TVersionedNodeId(Id_, TransactionId_);
}

bool TCypressNodeBase::IsExternal() const
{
    return ExternalCellTag_ >= MinValidCellTag && ExternalCellTag_ <= MaxValidCellTag;
}

void TCypressNodeBase::Save(TSaveContext& context) const
{
    TObjectBase::Save(context);

    using NYT::Save;
    Save(context, ExternalCellTag_);
    Save(context, AccountingEnabled_);
    Save(context, LockStateMap_);
    Save(context, AcquiredLocks_);
    Save(context, PendingLocks_);
    TNonversionedObjectRefSerializer::Save(context, Parent_);
    Save(context, LockMode_);
    Save(context, ExpirationTime_);
    Save(context, CreationTime_);
    Save(context, ModificationTime_);
    Save(context, Revision_);
    Save(context, Account_);
    Save(context, CachedResourceUsage_);
    Save(context, Acd_);
    Save(context, AccessTime_);
    Save(context, AccessCounter_);
}

void TCypressNodeBase::Load(TLoadContext& context)
{
    TObjectBase::Load(context);

    using NYT::Load;
    // COMPAT(babenko)
    if (context.GetVersion() >= 200) {
        Load(context, ExternalCellTag_);
        Load(context, AccountingEnabled_);
    }
    Load(context, LockStateMap_);
    Load(context, AcquiredLocks_);
    Load(context, PendingLocks_);
    TNonversionedObjectRefSerializer::Load(context, Parent_);
    Load(context, LockMode_);
    // COMPAT(babenko)
    if (context.GetVersion() >= 211) {
        Load(context, ExpirationTime_);
    }
    Load(context, CreationTime_);
    Load(context, ModificationTime_);
    Load(context, Revision_);
    Load(context, Account_);
    Load(context, CachedResourceUsage_);
    Load(context, Acd_);
    Load(context, AccessTime_);
    Load(context, AccessCounter_);
}

TVersionedObjectId GetObjectId(const TCypressNodeBase* object)
{
    return object ? object->GetVersionedId() : TVersionedObjectId(NullObjectId, NullTransactionId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT

