#include "tablet_type_handler.h"
#include "table_replica_proxy.h"
#include "table_replica.h"
#include "tablet_manager.h"

#include <yt/server/object_server/type_handler_detail.h>

#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/table_server/replicated_table_node.h>

#include <yt/ytlib/object_client/helpers.h>

namespace NYT {
namespace NTabletServer {

using namespace NHydra;
using namespace NYTree;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NCypressServer;
using namespace NTableServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TTableReplicaTypeHandler
    : public TObjectTypeHandlerWithMapBase<TTableReplica>
{
public:
    TTableReplicaTypeHandler(
        TBootstrap* bootstrap,
        TEntityMap<TTableReplica>* map)
        : TObjectTypeHandlerWithMapBase(bootstrap, map)
        , Bootstrap_(bootstrap)
    { }

    virtual EObjectType GetType() const override
    {
        return EObjectType::TableReplica;
    }

    virtual ETypeFlags GetFlags() const override
    {
        return ETypeFlags::Creatable;
    }

    virtual TObjectBase* CreateObject(
        const TObjectId& hintId,
        IAttributeDictionary* attributes) override
    {
        auto tablePath = attributes->GetAndRemove<Stroka>("table_path");
        auto clusterName = attributes->GetAndRemove<Stroka>("cluster_name");
        auto replicaPath = attributes->GetAndRemove<Stroka>("replica_path");
        auto startReplicationTimestamp = attributes->GetAndRemove<NTransactionClient::TTimestamp>("start_replication_timestamp", NTransactionClient::MinTimestamp);
        auto mode = attributes->GetAndRemove<ETableReplicaMode>("mode", ETableReplicaMode::Async);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto resolver = cypressManager->CreateResolver(nullptr);
        auto nodeProxy = resolver->ResolvePath(tablePath);

        auto* cypressNodeProxy = ICypressNodeProxy::FromNode(nodeProxy.Get());
        if (cypressNodeProxy->GetTrunkNode()->GetType() != EObjectType::ReplicatedTable) {
            THROW_ERROR_EXCEPTION("%v is not a replicated table",
                tablePath);
        }
        auto* table = cypressNodeProxy->GetTrunkNode()->As<TReplicatedTableNode>();

        cypressManager->LockNode(table, nullptr, ELockMode::Exclusive);

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        return tabletManager->CreateTableReplica(
            table,
            clusterName,
            replicaPath,
            mode,
            startReplicationTimestamp);
    }

private:
    TBootstrap* const Bootstrap_;

    virtual Stroka DoGetName(const TTableReplica* replica) override
    {
        return Format("table replica %v", replica->GetId());
    }

    virtual IObjectProxyPtr DoGetProxy(TTableReplica* replica, TTransaction* /*transaction*/) override
    {
        return CreateTableReplicaProxy(Bootstrap_, &Metadata_, replica);
    }

    virtual void DoZombifyObject(TTableReplica* replica) override
    {
        TObjectTypeHandlerWithMapBase::DoDestroyObject(replica);
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->DestroyTableReplica(replica);
    }
};

IObjectTypeHandlerPtr CreateTableReplicaTypeHandler(
    TBootstrap* bootstrap,
    TEntityMap<TTableReplica>* map)
{
    return New<TTableReplicaTypeHandler>(bootstrap, map);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
