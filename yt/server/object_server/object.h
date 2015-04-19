#pragma once

#include "public.h"
#include "attribute_set.h"

#include <server/hydra/entity_map.h>

#include <server/cell_master/public.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

//! Provides a base for all objects in YT master server.
class TObjectBase
    : public NHydra::TEntityBase
{
public:
    explicit TObjectBase(const TObjectId& id);

    //! Returns the object id.
    const TObjectId& GetId() const;

    //! Returns the object type.
    EObjectType GetType() const;

    //! Returns |true| if this is a well-known subject (e.g. "root", "users" etc).
    bool IsBuiltin() const;


    //! Increments the object's reference counter.
    /*!
     *  \returns the incremented counter.
     */
    int RefObject();

    //! Decrements the object's reference counter.
    /*!
     *  \note
     *  Objects do not self-destruct, it's callers responsibility to check
     *  if the counter reaches zero.
     *
     *  \returns the decremented counter.
     */
    int UnrefObject();

    //! Increments the object's weak reference counter.
    /*!
     *  \returns the incremented counter.
     */
    int WeakRefObject();

    //! Decrements the object's weak reference counter.
    /*!
     *  \returns the decremented counter.
     */
    int WeakUnrefObject();


    //! Sets weak reference counter to zero.
    void ResetWeakRefCounter();

    //! Returns the current reference counter.
    int GetObjectRefCounter() const;

    //! Returns the current lock counter.
    int GetObjectWeakRefCounter() const;


    //! Returns |true| iff the reference counter is non-zero.
    bool IsAlive() const;

    //! Returns |true| iff the lock counter is non-zero.
    bool IsLocked() const;

    //! Returns |true| iff the object is either non-versioned or versioned but does not belong to a transaction.
    bool IsTrunk() const;


    //! Returns an immutable collection of attributes associated with the object or |nullptr| is there are none.
    const TAttributeSet* GetAttributes() const;

    //! Returns (created if needed) a mutable collection of attributes associated with the object.
    TAttributeSet* GetMutableAttributes();

    //! Clears the collection of attributes associated with the object.
    void ClearAttributes();

protected:
    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    TObjectId Id;
    int RefCounter = 0;
    int WeakRefCounter = 0;
    std::unique_ptr<TAttributeSet> Attributes_;

};

TObjectId GetObjectId(const TObjectBase* object);
bool IsObjectAlive(const TObjectBase* object);

////////////////////////////////////////////////////////////////////////////////

template <class T>
std::vector<TObjectId> ToObjectIds(
    const T& objects,
    size_t sizeLimit = std::numeric_limits<size_t>::max())
{
    std::vector<TObjectId> result;
    result.reserve(std::min(objects.size(), sizeLimit));
    for (auto* object : objects) {
        if (result.size() == sizeLimit)
            break;
        result.push_back(object->GetId());
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

class TNonversionedObjectBase
    : public TObjectBase
{
public:
    explicit TNonversionedObjectBase(const TObjectId& id);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

