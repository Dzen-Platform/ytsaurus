#include "stdafx.h"
#include "object.h"

#include <ytlib/object_client/helpers.h>

#include <server/cypress_server/node.h>

#include <server/cell_master/serialize.h>

namespace NYT {
namespace NObjectServer {

using namespace NObjectClient;
using namespace NCypressServer;

////////////////////////////////////////////////////////////////////////////////

TObjectBase::TObjectBase(const TObjectId& id)
    : Id_(id)
{ }

TObjectBase::~TObjectBase()
{
    // To make debugging easier.
    RefCounter_ = DisposedRefCounter;
}

void TObjectBase::SetDestroyed()
{
    YASSERT(RefCounter_ == 0);
    RefCounter_ = DestroyedRefCounter;
}

const TObjectId& TObjectBase::GetId() const
{
    return Id_;
}

EObjectType TObjectBase::GetType() const
{
    return TypeFromId(Id_);
}

bool TObjectBase::IsBuiltin() const
{
    return IsWellKnownId(Id_);
}

int TObjectBase::RefObject()
{
    YASSERT(RefCounter_ >= 0);
    return ++RefCounter_;
}

int TObjectBase::UnrefObject()
{
    YASSERT(RefCounter_ > 0);
    return --RefCounter_;
}

int TObjectBase::WeakRefObject()
{
    YCHECK(IsAlive());
    YASSERT(WeakRefCounter_ >= 0);
    return ++WeakRefCounter_;
}

int TObjectBase::WeakUnrefObject()
{
    YASSERT(WeakRefCounter_ > 0);
    return --WeakRefCounter_;
}

void TObjectBase::ResetWeakRefCounter()
{
    WeakRefCounter_ = 0;
}

int TObjectBase::GetObjectRefCounter() const
{
    return RefCounter_;
}

int TObjectBase::GetObjectWeakRefCounter() const
{
    return WeakRefCounter_;
}

bool TObjectBase::IsAlive() const
{
    return RefCounter_ > 0;
}

bool TObjectBase::IsDestroyed() const
{
    return RefCounter_ == DestroyedRefCounter;
}

bool TObjectBase::IsLocked() const
{
    return WeakRefCounter_ > 0;
}

bool TObjectBase::IsTrunk() const
{
    if (!IsVersionedType(TypeFromId(Id_))) {
        return true;
    }

    auto* node = static_cast<const TCypressNodeBase*>(this);
    return node->GetTrunkNode() == node;
}

const TAttributeSet* TObjectBase::GetAttributes() const
{
    return Attributes_.get();
}

TAttributeSet* TObjectBase::GetMutableAttributes()
{
    if (!Attributes_) {
        Attributes_ = std::make_unique<TAttributeSet>();
    }
    return Attributes_.get();
}

void TObjectBase::ClearAttributes()
{
    Attributes_.reset();
}

void TObjectBase::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, RefCounter_);
    if (Attributes_) {
        Save(context, true);
        Save(context, *Attributes_);
    } else {
        Save(context, false);
    }
}

void TObjectBase::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, RefCounter_);
    // COMPAT(babenko)
    if (context.GetVersion() >= 117) {
        if (Load<bool>(context)) {
            Attributes_ = std::make_unique<TAttributeSet>();
            Load(context, *Attributes_);
        }
    }
}

TObjectId GetObjectId(const TObjectBase* object)
{
    return object ? object->GetId() : NullObjectId;
}

bool IsObjectAlive(const TObjectBase* object)
{
    return object && object->IsAlive();
}

////////////////////////////////////////////////////////////////////////////////

TNonversionedObjectBase::TNonversionedObjectBase(const TObjectId& id)
    : TObjectBase(id)
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
