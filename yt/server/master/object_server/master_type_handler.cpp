#include "master_type_handler.h"
#include "type_handler_detail.h"
#include "master.h"
#include "master_proxy.h"

namespace NYT::NObjectServer {

using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TMasterTypeHandler
    : public TObjectTypeHandlerBase<TMasterObject>
{
public:
    explicit TMasterTypeHandler(TBootstrap* bootstrap)
        : TObjectTypeHandlerBase(bootstrap)
    { }

    virtual EObjectType GetType() const override
    {
        return EObjectType::Master;
    }

    virtual TObjectBase* FindObject(TObjectId id) override
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto* object = objectManager->GetMasterObject();
        return id == object->GetId() ? object : nullptr;
    }

    virtual void DestroyObject(TObjectBase* /*object*/) noexcept override
    {
        YT_ABORT();
    }

private:
    virtual TString DoGetName(const TMasterObject* /*object*/) override
    {
        return "master";
    }

    virtual IObjectProxyPtr DoGetProxy(
        TMasterObject* object,
        NTransactionServer::TTransaction* /*transaction*/) override
    {
        return CreateMasterProxy(Bootstrap_, &Metadata_, object);
    }
};

IObjectTypeHandlerPtr CreateMasterTypeHandler(TBootstrap* bootstrap)
{
    return New<TMasterTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
