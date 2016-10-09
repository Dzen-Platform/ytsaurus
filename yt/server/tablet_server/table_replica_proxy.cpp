#include "table_replica_proxy.h"
#include "table_replica.h"
#include "tablet_manager.h"

#include <yt/server/object_server/object_detail.h>

#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/table_server/replicated_table_node.h>

#include <yt/server/cell_master/bootstrap.h>

#include <yt/ytlib/tablet_client/table_replica_ypath.pb.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NTabletServer {

using namespace NYTree;
using namespace NYson;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TTableReplicaProxy
    : public TNonversionedObjectProxyBase<TTableReplica>
{
public:
    TTableReplicaProxy(
        NCellMaster::TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TTableReplica* replica)
        : TBase(bootstrap, metadata, replica)
    { }

private:
    typedef TNonversionedObjectProxyBase<TTableReplica> TBase;

    virtual void ValidateRemoval() override
    { }

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* attributes) override
    {
        attributes->push_back("cluster_name");
        attributes->push_back("replica_path");
        attributes->push_back("table_path");
        attributes->push_back("start_replication_timestamp");
        attributes->push_back("state");
        attributes->push_back(TAttributeDescriptor("tablets")
            .SetOpaque(true));
        attributes->push_back(TAttributeDescriptor("replication_lag_time")
            .SetOpaque(true));

        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        auto* replica = GetThisImpl();
        auto* table = replica->GetTable();

        if (key == "cluster_name") {
            BuildYsonFluently(consumer)
                .Value(replica->GetClusterName());
            return true;
        }

        if (key == "replica_path") {
            BuildYsonFluently(consumer)
                .Value(replica->GetReplicaPath());
            return true;
        }

        if (key == "start_replication_timestamp") {
            BuildYsonFluently(consumer)
                .Value(replica->GetStartReplicationTimestamp());
            return true;
        }

        if (key == "table_path") {
            auto cypressManager = Bootstrap_->GetCypressManager();
            auto tableProxy = cypressManager->GetNodeProxy(replica->GetTable(), nullptr);
            BuildYsonFluently(consumer)
                .Value(tableProxy->GetPath());
            return true;
        }

        if (key == "state") {
            BuildYsonFluently(consumer)
                .Value(replica->GetState());
            return true;
        }

        if (key == "tablets") {
            BuildYsonFluently(consumer)
                .DoMapFor(table->Tablets(), [=] (TFluentMap fluent, TTablet* tablet) {
                    const auto& replicaInfo = tablet->GetReplicaInfo(replica);
                    fluent
                        .Item(ToString(tablet->GetId()))
                        .BeginMap()
                            .Item("state").Value(replicaInfo->GetState())
                            .Item("current_replication_row_index").Value(replicaInfo->GetCurrentReplicationRowIndex())
                            .Item("current_replication_timestamp").Value(replicaInfo->GetCurrentReplicationTimestamp())
                            .Item("replication_lag_time").Value(tablet->ComputeReplicationLagTime(*replicaInfo))
                        .EndMap();
                });
            return true;
        }

        if (key == "replication_lag_time") {
            BuildYsonFluently(consumer)
                .Value(replica->ComputeReplicationLagTime());
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override
    {
        DISPATCH_YPATH_SERVICE_METHOD(Enable);
        DISPATCH_YPATH_SERVICE_METHOD(Disable);
        return TBase::DoInvoke(context);
    }

    DECLARE_YPATH_SERVICE_METHOD(NTabletClient::NProto, Enable)
    {
        Y_UNUSED(request);
        Y_UNUSED(response);

        DeclareMutating();

        auto* replica = GetThisImpl();

        auto tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->EnableTableReplica(replica);

        context->Reply();
    }

    DECLARE_YPATH_SERVICE_METHOD(NTabletClient::NProto, Disable)
    {
        Y_UNUSED(request);
        Y_UNUSED(response);

        DeclareMutating();

        auto* replica = GetThisImpl();

        auto tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->DisableTableReplica(replica);

        context->Reply();
    }
};

IObjectProxyPtr CreateTableReplicaProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTableReplica* replica)
{
    return New<TTableReplicaProxy>(bootstrap, metadata, replica);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

