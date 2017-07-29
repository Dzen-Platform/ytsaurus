#include "table_replica_proxy.h"
#include "table_replica.h"
#include "tablet_manager.h"

#include <yt/server/object_server/object_detail.h>

#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/chunk_server/chunk_list.h>

#include <yt/server/table_server/replicated_table_node.h>

#include <yt/server/cell_master/bootstrap.h>

#include <yt/ytlib/tablet_client/table_replica_ypath.pb.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NTabletServer {

using namespace NYTree;
using namespace NYson;
using namespace NTabletClient;
using namespace NObjectServer;
using namespace NCypressServer;

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
    {
        auto* replica = GetThisImpl();
        auto* table = replica->GetTable();
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        cypressManager->LockNode(table, nullptr, TLockRequest(ELockMode::Exclusive));
    }

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* attributes) override
    {
        attributes->push_back("cluster_name");
        attributes->push_back("replica_path");
        attributes->push_back("table_path");
        attributes->push_back("start_replication_timestamp");
        attributes->push_back("state");
        attributes->push_back("mode");
        attributes->push_back(TAttributeDescriptor("tablets")
            .SetOpaque(true));
        attributes->push_back(TAttributeDescriptor("replication_lag_time")
            .SetOpaque(true));

        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetBuiltinAttribute(const TString& key, IYsonConsumer* consumer) override
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
            const auto& cypressManager = Bootstrap_->GetCypressManager();
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

        if (key == "mode") {
            BuildYsonFluently(consumer)
                .Value(replica->GetMode());
            return true;
        }

        if (key == "tablets") {
            BuildYsonFluently(consumer)
                .DoListFor(table->Tablets(), [=] (TFluentList fluent, TTablet* tablet) {
                    const auto* chunkList = tablet->GetChunkList();
                    const auto* replicaInfo = tablet->GetReplicaInfo(replica);
                    fluent
                        .Item().BeginMap()
                            .Item("tablet_id").Value(tablet->GetId())
                            .Item("state").Value(replicaInfo->GetState())
                            .Item("current_replication_row_index").Value(replicaInfo->GetCurrentReplicationRowIndex())
                            .Item("current_replication_timestamp").Value(replicaInfo->GetCurrentReplicationTimestamp())
                            .Item("replication_lag_time").Value(tablet->ComputeReplicationLagTime(*replicaInfo))
                            .DoIf(!replicaInfo->Error().IsOK(), [&] (TFluentMap fluent) {
                                fluent.Item("replication_error").Value(replicaInfo->Error());
                            })
                            .Item("trimmed_row_count").Value(tablet->GetTrimmedRowCount())
                            .Item("flushed_row_count").Value(chunkList->Statistics().LogicalRowCount)
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

    virtual bool DoInvoke(const NRpc::IServiceContextPtr& context) override
    {
        DISPATCH_YPATH_SERVICE_METHOD(Alter);
        return TBase::DoInvoke(context);
    }

    DECLARE_YPATH_SERVICE_METHOD(NTabletClient::NProto, Alter)
    {
        Y_UNUSED(response);

        DeclareMutating();

        auto enabled = request->has_enabled() ? MakeNullable(request->enabled()) : Null;
        auto mode = request->has_mode() ? MakeNullable(ETableReplicaMode(request->mode())) : Null;

        context->SetRequestInfo("Enabled: %v, Mode: %v",
            enabled,
            mode);

        auto* replica = GetThisImpl();

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        if (enabled) {
            tabletManager->SetTableReplicaEnabled(replica, *enabled);
        }
        if (mode) {
            tabletManager->SetTableReplicaMode(replica, *mode);
        }

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

