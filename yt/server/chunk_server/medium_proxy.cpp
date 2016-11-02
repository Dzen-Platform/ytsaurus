#include "medium_proxy.h"
#include "chunk_manager.h"
#include "medium.h"

#include <yt/server/cell_master/bootstrap.h>

#include <yt/server/object_server/object_detail.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NChunkServer {

using namespace NYTree;
using namespace NYson;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TMediumProxy
    : public TNonversionedObjectProxyBase<TMedium>
{
public:
    TMediumProxy(
        NCellMaster::TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TMedium* medium)
        : TBase(bootstrap, metadata, medium)
    { }

private:
    typedef TNonversionedObjectProxyBase<TMedium> TBase;

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        descriptors->push_back(TAttributeDescriptor("name")
            .SetReplicated(true)
            .SetMandatory(true));
        descriptors->push_back(TAttributeDescriptor("index")
            .SetMandatory(true));
        descriptors->push_back(TAttributeDescriptor("transient")
            .SetReplicated(true));
        descriptors->push_back(TAttributeDescriptor("cache")
            .SetReplicated(true));
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override
    {
        const auto* medium = GetThisImpl();

        if (key == "name") {
            BuildYsonFluently(consumer)
                .Value(medium->GetName());
            return true;
        }

        if (key == "index") {
            BuildYsonFluently(consumer)
                .Value(medium->GetIndex());
            return true;
        }

        if (key == "transient") {
            BuildYsonFluently(consumer)
                .Value(medium->GetTransient());
            return true;
        }

        if (key == "cache") {
            BuildYsonFluently(consumer)
                .Value(medium->GetCache());
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual bool SetBuiltinAttribute(const Stroka& key, const TYsonString& value) override
    {
        auto* medium = GetThisImpl();
        auto chunkManager = Bootstrap_->GetChunkManager();

        if (key == "name") {
            auto newName = ConvertTo<Stroka>(value);
            chunkManager->RenameMedium(medium, newName);
            return true;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }
};

IObjectProxyPtr CreateMediumProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TMedium* medium)
{
    return New<TMediumProxy>(bootstrap, metadata, medium);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
