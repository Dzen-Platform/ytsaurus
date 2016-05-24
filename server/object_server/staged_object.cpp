#include "staged_object.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/security_server/account.h>

#include <yt/server/transaction_server/transaction.h>

#include <yt/core/misc/common.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

TStagedObject::TStagedObject(const TObjectId& id)
    : TNonversionedObjectBase(id)
    , StagingTransaction_(nullptr)
    , StagingAccount_(nullptr)
{ }

void TStagedObject::Save(NCellMaster::TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);

    using NYT::Save;
    Save(context, StagingTransaction_);
    Save(context, StagingAccount_);
}

void TStagedObject::Load(NCellMaster::TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);

    // COMPAT(babenko)
    if (GetType() != EObjectType::ChunkList || context.GetVersion() >= 100) {
        using NYT::Load;
        Load(context, StagingTransaction_);
        Load(context, StagingAccount_);
    }
}

bool TStagedObject::IsStaged() const
{
    return StagingTransaction_ && StagingAccount_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
