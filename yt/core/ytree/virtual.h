#pragma once

#include "system_attribute_provider.h"
#include "ypath_detail.h"

#include <yt/core/yson/producer.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TVirtualMapBase
    : public TSupportsAttributes
    , public ISystemAttributeProvider
{
protected:
    explicit TVirtualMapBase(INodePtr owningNode = nullptr);

    virtual std::vector<Stroka> GetKeys(i64 limit = std::numeric_limits<i64>::max()) const = 0;
    virtual i64 GetSize() const = 0;
    virtual IYPathServicePtr FindItemService(const TStringBuf& key) const = 0;

    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override;

    virtual TResolveResult ResolveRecursive(const TYPath& path, NRpc::IServiceContextPtr context) override;
    virtual void GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context) override;
    virtual void ListSelf(TReqList* request, TRspList* response, TCtxListPtr context) override;

    // TSupportsAttributes overrides
    virtual ISystemAttributeProvider* GetBuiltinAttributeProvider() override;

    // ISystemAttributeProvider overrides
    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override;
    virtual const yhash_set<const char*>& GetBuiltinAttributeKeys() override;
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual TFuture<NYson::TYsonString> GetBuiltinAttributeAsync(const Stroka& key) override;
    virtual bool SetBuiltinAttribute(const Stroka& key, const NYson::TYsonString& value) override;
    virtual bool RemoveBuiltinAttribute(const Stroka& key) override;

private:
    const INodePtr OwningNode_;

    TBuiltinAttributeKeysCache BuiltinAttributeKeysCache_;

};

////////////////////////////////////////////////////////////////////////////////

class TCompositeMapService
    : public TVirtualMapBase
{
public:
    TCompositeMapService();

    virtual std::vector<Stroka> GetKeys(i64 limit = std::numeric_limits<i64>::max()) const override;
    virtual i64 GetSize() const override;
    virtual IYPathServicePtr FindItemService(const TStringBuf& key) const override;
    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override;
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;

    TIntrusivePtr<TCompositeMapService> AddChild(const Stroka& key, IYPathServicePtr service);
    TIntrusivePtr<TCompositeMapService> AddAttribute(const Stroka& key, NYson::TYsonCallback producer);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

INodePtr CreateVirtualNode(IYPathServicePtr service);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
