#include "virtual_service_type_handler.h"
#include "type_handler_detail.h"
#include "virtual_service.h"
#include "db_schema.h"

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

class TVirtualServiceTypeHandler
    : public TObjectTypeHandlerBase
{
public:
    explicit TVirtualServiceTypeHandler(NMaster::TBootstrap* bootstrap)
        : TObjectTypeHandlerBase(bootstrap, EObjectType::VirtualService)
    { }

    virtual void Initialize() override
    {
        TObjectTypeHandlerBase::Initialize();

        SpecAttributeSchema_
            ->SetAttribute(TVirtualService::SpecSchema);

        StatusAttributeSchema_
            ->SetComposite();
    }

    virtual const NYson::TProtobufMessageType* GetRootProtobufType() override
    {
        return NYson::ReflectProtobufMessageType<NClient::NApi::NProto::TVirtualService>();
    }

    virtual const TDBTable* GetTable() override
    {
        return &VirtualServicesTable;
    }

    virtual const TDBField* GetIdField() override
    {
        return &VirtualServicesTable.Fields.Meta_Id;
    }

    virtual std::unique_ptr<TObject> InstantiateObject(
        const TObjectId& id,
        const TObjectId& parentId,
        ISession* session) override
    {
        YT_VERIFY(!parentId);
        return std::unique_ptr<TObject>(new TVirtualService(id, this, session));
    }
};

std::unique_ptr<IObjectTypeHandler> CreateVirtualServiceTypeHandler(NMaster::TBootstrap* bootstrap)
{
    return std::unique_ptr<IObjectTypeHandler>(new TVirtualServiceTypeHandler(bootstrap));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects

