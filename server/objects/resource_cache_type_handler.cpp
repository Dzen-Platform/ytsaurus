#include "resource_type_handler.h"
#include "type_handler_detail.h"
#include "replica_set.h"
#include "resource_cache.h"
#include "db_schema.h"

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

class TResourceCacheTypeHandler
    : public TObjectTypeHandlerBase
{
public:
    explicit TResourceCacheTypeHandler(NMaster::TBootstrap* bootstrap)
        : TObjectTypeHandlerBase(bootstrap, EObjectType::ResourceCache)
    {
        MetaAttributeSchema_
            ->AddChildren({
                ParentIdAttributeSchema_ = MakeAttributeSchema("replica_set_id")
                    ->SetParentAttribute()
                    ->SetMandatory()
            });

        SpecAttributeSchema_
            ->SetAttribute(TResourceCache::SpecSchema);

        StatusAttributeSchema_
            ->SetAttribute(TResourceCache::StatusSchema)
            ->SetUpdatable();
    }

    virtual const NYson::TProtobufMessageType* GetRootProtobufType() override
    {
        return NYson::ReflectProtobufMessageType<NClient::NApi::NProto::TResourceCache>();
    }

    virtual EObjectType GetParentType() override
    {
        return EObjectType::ReplicaSet;
    }

    virtual TObject* GetParent(TObject* object) override
    {
        return object->As<TResourceCache>()->ReplicaSet().Load();
    }

    virtual const TDBField* GetIdField() override
    {
        return &ResourceCachesTable.Fields.Meta_Id;
    }

    virtual const TDBField* GetParentIdField() override
    {
        return &ResourceCachesTable.Fields.Meta_ReplicaSetId;
    }

    virtual const TDBTable* GetTable() override
    {
        return &ResourceCachesTable;
    }

    virtual TChildrenAttributeBase* GetParentChildrenAttribute(TObject* parent) override
    {
        return &parent->As<TReplicaSet>()->ResourceCache();
    }

    virtual std::unique_ptr<TObject> InstantiateObject(
        const TObjectId& id,
        const TObjectId& parentId,
        ISession* session) override
    {
        return std::make_unique<TResourceCache>(id, parentId, this, session);
    }
};

std::unique_ptr<IObjectTypeHandler> CreateResourceCacheTypeHandler(NMaster::TBootstrap* bootstrap)
{
    return std::unique_ptr<IObjectTypeHandler>(new TResourceCacheTypeHandler(bootstrap));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects

