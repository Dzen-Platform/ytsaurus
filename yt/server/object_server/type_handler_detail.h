#pragma once

#include "type_handler.h"
#include "object_detail.h"
#include "object_manager.h"

#include <server/hydra/entity_map.h>

#include <server/cell_master/bootstrap.h>

#include <server/transaction_server/public.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

template <class TObject>
class TObjectTypeHandlerBase
    : public IObjectTypeHandler
{
public:
    explicit TObjectTypeHandlerBase(NCellMaster::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    {
        YCHECK(bootstrap);
    }

    virtual Stroka GetName(TObjectBase* object) override
    {
        return DoGetName(static_cast<TObject*>(object));
    }

    virtual IObjectProxyPtr GetProxy(
        TObjectBase* object,
        NTransactionServer::TTransaction* transaction) override
    {
        return DoGetProxy(static_cast<TObject*>(object), transaction);
    }

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return Null;
    }

    virtual TObjectBase* CreateObject(
        NTransactionServer::TTransaction* /*transaction*/,
        NSecurityServer::TAccount* /*account*/,
        NYTree::IAttributeDictionary* /*attributes*/,
        TReqCreateObjects* /*request*/,
        TRspCreateObjects* /*response*/) override
    {
        YUNREACHABLE();
    }

    virtual void ZombifyObject(TObjectBase* object) throw() override
    {
        DoZombifyObject(static_cast<TObject*>(object));
    }

    virtual NTransactionServer::TTransaction* GetStagingTransaction(
        TObjectBase* object) override
    {
        return DoGetStagingTransaction(static_cast<TObject*>(object));
    }

    virtual void UnstageObject(TObjectBase* object, bool recursive) override
    {
        DoUnstageObject(static_cast<TObject*>(object), recursive);
    }

    virtual NSecurityServer::TAccessControlDescriptor* FindAcd(TObjectBase* object) override
    {
        return DoFindAcd(static_cast<TObject*>(object));
    }

    virtual TObjectBase* GetParent(TObjectBase* object) override
    {
        return DoGetParent(static_cast<TObject*>(object));
    }

    virtual NYTree::EPermissionSet GetSupportedPermissions() const override
    {
        return
            NYTree::EPermissionSet::Read |
            NYTree::EPermissionSet::Write |
            NYTree::EPermissionSet::Remove |
            NYTree::EPermissionSet::Administer;
    }

protected:
    NCellMaster::TBootstrap* const Bootstrap_;

    virtual Stroka DoGetName(TObject* object) = 0;

    virtual IObjectProxyPtr DoGetProxy(
        TObject* object,
        NTransactionServer::TTransaction* /*transaction*/)
    {
        return New<TNonversionedObjectProxyBase<TObject>>(Bootstrap_, object);
    }

    virtual void DoZombifyObject(TObject* /*object*/)
    { }

    virtual NTransactionServer::TTransaction* DoGetStagingTransaction(
        TObject* /*object*/)
    {
        return nullptr;
    }

    virtual void DoUnstageObject(TObject* /*object*/, bool /*recursive*/)
    { }

    virtual NSecurityServer::TAccessControlDescriptor* DoFindAcd(TObject* /*object*/)
    {
        return nullptr;
    }

    virtual TObjectBase* DoGetParent(TObject* /*object*/)
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        return objectManager->FindSchema(GetType());
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TObject>
class TObjectTypeHandlerWithMapBase
    : public TObjectTypeHandlerBase<TObject>
{
public:
    typedef typename NHydra::TEntityMap<TObjectId, TObject> TMap;

    TObjectTypeHandlerWithMapBase(NCellMaster::TBootstrap* bootstrap, TMap* map)
        : TObjectTypeHandlerBase<TObject>(bootstrap)
        , Map_(map)
    { }

    virtual void DestroyObject(TObjectBase* object) throw() override
    {
        this->DoDestroyObject(static_cast<TObject*>(object));
    	// Remove the object from the map but keep it alive.
        Map_->Release(object->GetId()).release();
    }

    virtual NObjectServer::TObjectBase* FindObject(const TObjectId& id) override
    {
        return Map_->Find(id);
    }

    virtual void ResetAllObjects() override
    {
        for (const auto& pair : *Map_) {
            auto* object = pair.second;
            this->DoResetObject(object);
        }
    }

protected:
    virtual void DoDestroyObject(TObject* object)
    {
        // Clear ACD, if any.
        auto* acd = this->FindAcd(object);
        if (acd) {
            acd->Clear();
        }
    }

    virtual void DoResetObject(TObject* object)
    {
        object->ResetWeakRefCounter();
    }

private:
    // We store map by a raw pointer. In most cases this should be OK.
    TMap* const Map_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

