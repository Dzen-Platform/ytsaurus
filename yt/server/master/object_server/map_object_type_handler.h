#pragma once

#include "type_handler.h"
#include "type_handler_detail.h"

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>

namespace NYT::NObjectServer {

////////////////////////////////////////////////////////////////////////////////

template <class TObject>
class TNonversionedMapObjectProxyBase;

////////////////////////////////////////////////////////////////////////////////

template <class TObject>
class TNonversionedMapObjectTypeHandlerBase
    : public TObjectTypeHandlerWithMapBase<TObject>
{
protected:
    using TProxyPtr = TIntrusivePtr<TNonversionedMapObjectProxyBase<TObject>>;

private:
    using TMapType = NHydra::TEntityMap<TObject>;
    using TBase = NObjectServer::TObjectTypeHandlerWithMapBase<TObject>;

public:
    TNonversionedMapObjectTypeHandlerBase(NCellMaster::TBootstrap* bootstrap, TMapType* map);

    virtual ETypeFlags GetFlags() const override;

    virtual NObjectServer::TObject* DoGetParent(TObject* object) override;

    // XXX(kiselyovp) Should this be a pure virtual method?
    //! Returns Cypress path to a map object which must be a designated root.
    virtual TString GetRootPath(const TObject* rootObject) const;

    virtual void RegisterName(const TString& /* name */, TObject* /* object */) noexcept = 0;
    virtual void UnregisterName(const TString& /* name */, TObject* /* object */) noexcept = 0;

    virtual void ValidateObjectName(const TString& name);

protected:
    virtual IObjectProxyPtr DoGetProxy(TObject* object, NTransactionServer::TTransaction* transaction) override;
    virtual TString DoGetName(const TObject* object) override;
    virtual NSecurityServer::TAccessControlDescriptor* DoFindAcd(TObject* object) override;
    virtual void DoZombifyObject(TObject* object) override;

    virtual TProxyPtr GetMapObjectProxy(TObject* object) = 0;

    NObjectServer::TObject* CreateObjectImpl(
        const TString& name,
        TObject* parent,
        NYTree::IAttributeDictionary* attributes);

    virtual std::optional<int> GetDepthLimit() const;

    friend class TNonversionedMapObjectProxyBase<TObject>;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
