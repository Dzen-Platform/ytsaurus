#pragma once

#include "type_handler.h"
#include "attribute_schema.h"
#include "private.h"

#include <yp/server/master/public.h>

#include <yp/server/access_control/public.h>

#include <yt/client/api/public.h>

namespace NYP {
namespace NServer {
namespace NObjects {

////////////////////////////////////////////////////////////////////////////////

class TObjectTypeHandlerBase
    : public IObjectTypeHandler
{
public:
    TObjectTypeHandlerBase(
        NMaster::TBootstrap* bootstrap,
        EObjectType type);

    virtual EObjectType GetType() override;
    virtual EObjectType GetParentType() override;
    virtual const TDBField* GetParentIdField() override;
    virtual TChildrenAttributeBase* GetParentChildrenAttribute(TObject* parent) override;

    virtual TObject* GetAccessControlParent(TObject* object) override;

    virtual TAttributeSchema* GetRootAttributeSchema() override;
    virtual TAttributeSchema* GetIdAttributeSchema() override;
    virtual TAttributeSchema* GetParentIdAttributeSchema() override;

    virtual void BeforeObjectCreated(
        TTransaction* transaction,
        TObject* object) override;

    virtual void AfterObjectCreated(
        TTransaction* transaction,
        TObject* object) override;

    virtual void BeforeObjectRemoved(
        TTransaction* transaction,
        TObject* object) override;

    virtual void AfterObjectRemoved(
        TTransaction* transaction,
        TObject* object) override;

protected:
    NMaster::TBootstrap* const Bootstrap_;
    const EObjectType Type_;

    const TObjectId SchemaId_;

    std::vector<std::unique_ptr<TAttributeSchema>> AttributeSchemas_;
    TAttributeSchema* RootAttributeSchema_ = nullptr;
    TAttributeSchema* IdAttributeSchema_ = nullptr;
    TAttributeSchema* ParentIdAttributeSchema_ = nullptr;
    TAttributeSchema* MetaAttributeSchema_ = nullptr;
    TAttributeSchema* SpecAttributeSchema_ = nullptr;
    TAttributeSchema* StatusAttributeSchema_ = nullptr;
    TAttributeSchema* AnnotationsAttributeSchema_ = nullptr;
    TAttributeSchema* ControlAttributeSchema_ = nullptr;

    TAttributeSchema* MakeAttributeSchema(const TString& name);
    TAttributeSchema* MakeFallbackAttributeSchema();

protected:
    virtual std::vector<NAccessControl::EAccessControlPermission> GetDefaultPermissions();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjects
} // namespace NServer
} // namespace NYP
