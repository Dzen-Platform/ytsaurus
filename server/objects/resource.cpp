#include "resource.h"
#include "node.h"
#include "db_schema.h"

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

const TScalarAttributeSchema<TResource, EResourceKind> TResource::KindSchema{
    &ResourcesTable.Fields.Meta_Kind,
    [] (TResource* resource) { return &resource->Kind(); }
};

const TScalarAttributeSchema<TResource, TResource::TSpec> TResource::SpecSchema{
    &ResourcesTable.Fields.Spec,
    [] (TResource* resource) { return &resource->Spec(); }
};

////////////////////////////////////////////////////////////////////////////////

const TScalarAttributeSchema<TResource, TResource::TStatus::TScheduledAllocations> TResource::TStatus::ScheduledAllocationsSchema{
    &ResourcesTable.Fields.Status_ScheduledAllocations,
    [] (TResource* resource) { return &resource->Status().ScheduledAllocations(); }
};

const TScalarAttributeSchema<TResource, TResource::TStatus::TActualAllocations> TResource::TStatus::ActualAllocationsSchema{
    &ResourcesTable.Fields.Status_ActualAllocations,
    [] (TResource* resource) { return &resource->Status().ActualAllocations(); }
};

TResource::TStatus::TStatus(TResource* resource)
    : ScheduledAllocations_(resource, &ScheduledAllocationsSchema)
    , ActualAllocations_(resource, &ActualAllocationsSchema)
{ }

////////////////////////////////////////////////////////////////////////////////

TResource::TResource(
    const TObjectId& id,
    const TObjectId& nodeId,
    IObjectTypeHandler* typeHandler,
    ISession* session)
    : TObject(id, nodeId, typeHandler, session)
    , Node_(this)
    , Kind_(this, &KindSchema)
    , Spec_(this, &SpecSchema)
    , Status_(this)
{ }

EObjectType TResource::GetType() const
{
    return EObjectType::Resource;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects

