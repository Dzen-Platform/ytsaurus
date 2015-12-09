#pragma once

#include "type_handler.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/cypress_server/public.h>

#include <yt/server/hydra/entity_map.h>

#include <yt/server/object_server/public.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

DEFINE_BIT_ENUM(EVirtualNodeOptions,
    ((None)            (0x0000))
    ((RequireLeader)   (0x0001))
    ((RedirectSelf)    (0x0002))
);

typedef
    TCallback< NYTree::IYPathServicePtr(TCypressNodeBase*, NTransactionServer::TTransaction*) >
    TYPathServiceProducer;

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    NCellMaster::TBootstrap* bootstrap,
    NObjectClient::EObjectType objectType,
    TYPathServiceProducer producer,
    EVirtualNodeOptions options);

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    NCellMaster::TBootstrap* bootstrap,
    NObjectClient::EObjectType objectType,
    NYTree::IYPathServicePtr service,
    EVirtualNodeOptions options);

template <
    class TId,
    class TValue
>
NYTree::IYPathServicePtr CreateVirtualObjectMap(
    NCellMaster::TBootstrap* bootstrap,
    const NHydra::TReadOnlyEntityMap<TId, TValue>& map);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT

#define VIRTUAL_INL_H_
#include "virtual-inl.h"
#undef VIRTUAL_INL_H_
