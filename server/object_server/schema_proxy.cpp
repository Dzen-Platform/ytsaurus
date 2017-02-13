#include "schema.h"
#include "private.h"
#include "type_handler.h"

#include <yt/server/cell_master/bootstrap.h>

#include <yt/server/object_server/type_handler_detail.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NObjectServer {

using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

class TSchemaProxy
    : public TNonversionedObjectProxyBase<TSchemaObject>
{
public:
    TSchemaProxy(
        NCellMaster::TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TSchemaObject* object)
        : TBase(bootstrap, metadata, object)
    { }

private:
    typedef TNonversionedObjectProxyBase<TSchemaObject> TBase;

    virtual bool GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        if (key == "type") {
            auto type = TypeFromSchemaType(TypeFromId(GetId()));
            BuildYsonFluently(consumer)
                .Value(Format("schema:%v", FormatEnum(type)));
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }
};

IObjectProxyPtr CreateSchemaProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TSchemaObject* object)
{
    return New<TSchemaProxy>(bootstrap, metadata, object);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
