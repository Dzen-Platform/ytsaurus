#include "query_service.h"
#include "public.h"
#include "private.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/query_agent/config.h>
#include <yt/server/query_agent/helpers.h>

#include <yt/server/tablet_node/security_manager.h>
#include <yt/server/tablet_node/slot_manager.h>
#include <yt/server/tablet_node/tablet.h>
#include <yt/server/tablet_node/tablet_manager.h>
#include <yt/server/tablet_node/transaction_manager.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/query_client/callbacks.h>
#include <yt/ytlib/query_client/query.h>
#include <yt/ytlib/query_client/query_service_proxy.h>
#include <yt/ytlib/query_client/query_statistics.h>
#include <yt/ytlib/query_client/functions_cache.h>

#include <yt/ytlib/table_client/schemaful_writer.h>

#include <yt/ytlib/tablet_client/wire_protocol.h>

#include <yt/core/compression/codec.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/rpc/service_detail.h>

namespace NYT {
namespace NQueryAgent {

using namespace NYTree;
using namespace NConcurrency;
using namespace NRpc;
using namespace NCompression;
using namespace NQueryClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletNode;
using namespace NCellNode;
using namespace NHydra;

////////////////////////////////////////////////////////////////////////////////

class TQueryService
    : public TServiceBase
{
public:
    TQueryService(
        TQueryAgentConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : TServiceBase(
            bootstrap->GetQueryPoolInvoker(),
            TQueryServiceProxy::GetDescriptor(),
            QueryAgentLogger)
        , Config_(config)
        , Bootstrap_(bootstrap)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Execute)
            .SetCancelable(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Read)
            .SetInvoker(bootstrap->GetLookupPoolInvoker()));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetTabletInfo)
            .SetInvoker(bootstrap->GetLookupPoolInvoker()));
    }

private:
    const TQueryAgentConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;


    DECLARE_RPC_SERVICE_METHOD(NQueryClient::NProto, Execute)
    {
        LOG_DEBUG("Deserializing subfragment");

        auto query = FromProto<TConstQueryPtr>(request->query());
        context->SetRequestInfo("FragmentId: %v", query->Id);

        auto externalCGInfo = New<TExternalCGInfo>();
        FromProto(&externalCGInfo->Functions, request->external_functions());
        externalCGInfo->NodeDirectory->MergeFrom(request->node_directory());

        auto options = FromProto<TQueryOptions>(request->options());

        auto dataSources = FromProto<std::vector<TDataRanges>>(request->data_sources());

        LOG_DEBUG("Deserialized subfragment (FragmentId: %v, InputRowLimit: %v, OutputRowLimit: %v, "
            "RangeExpansionLimit: %v, MaxSubqueries: %v, EnableCodeCache: %v, WorkloadDescriptor: %v, "
            "DataRangeCount: %v)",
            query->Id,
            query->InputRowLimit,
            query->OutputRowLimit,
            options.RangeExpansionLimit,
            options.MaxSubqueries,
            options.EnableCodeCache,
            options.WorkloadDescriptor,
            dataSources.size());

        const auto& user = context->GetUser();
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, user);

        ExecuteRequestWithRetries(
            Config_->MaxQueryRetries,
            Logger,
            [&] () {
                auto codecId = ECodec(request->response_codec());
                auto writer = CreateWireProtocolRowsetWriter(
                    codecId,
                    Config_->DesiredUncompressedResponseBlockSize,
                    query->GetTableSchema(),
                    false,
                    Logger);

                const auto& executor = Bootstrap_->GetQueryExecutor();
                auto asyncResult = executor->Execute(
                    query,
                    externalCGInfo,
                    dataSources,
                    writer,
                    options);
                auto result = WaitFor(asyncResult)
                    .ValueOrThrow();

                response->Attachments() = writer->GetCompressedBlocks();
                ToProto(response->mutable_query_statistics(), result);
                context->Reply();
            });
    }

    DECLARE_RPC_SERVICE_METHOD(NQueryClient::NProto, Read)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto mountRevision = request->mount_revision();
        auto timestamp = TTimestamp(request->timestamp());
        // TODO(sandello): Extract this out of RPC request.
        auto workloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::UserInteractive);
        auto requestCodecId = NCompression::ECodec(request->request_codec());
        auto responseCodecId = NCompression::ECodec(request->response_codec());

        context->SetRequestInfo("TabletId: %v, Timestamp: %llxx, RequestCodec: %v, ResponseCodec: %v",
            tabletId,
            timestamp,
            requestCodecId,
            responseCodecId);

        auto* requestCodec = NCompression::GetCodec(requestCodecId);
        auto* responseCodec = NCompression::GetCodec(responseCodecId);

        auto requestData = requestCodec->Decompress(request->Attachments()[0]);

        const auto& user = context->GetUser();
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, user);

        ExecuteRequestWithRetries(
            Config_->MaxQueryRetries,
            Logger,
            [&] () {
                const auto& slotManager = Bootstrap_->GetTabletSlotManager();
                auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tabletId);
                slotManager->ValidateTabletAccess(
                    tabletSnapshot,
                    EPermission::Read,
                    timestamp);

                tabletSnapshot->ValidateMountRevision(mountRevision);

                struct TLookupRowBufferTag { };
                TWireProtocolReader reader(requestData, New<TRowBuffer>(TLookupRowBufferTag()));
                TWireProtocolWriter writer;

                const auto& tabletManager = tabletSnapshot->TabletManager;
                tabletManager->Read(
                    tabletSnapshot,
                    timestamp,
                    workloadDescriptor,
                    &reader,
                    &writer);

                response->Attachments().push_back(responseCodec->Compress(writer.Finish()));
                context->Reply();
            });
    }

    DECLARE_RPC_SERVICE_METHOD(NQueryClient::NProto, GetTabletInfo)
    {
        auto tabletIds = FromProto<std::vector<TTabletId>>(request->tablet_ids());

        context->SetRequestInfo("TabletIds: %v", tabletIds);

        const auto& slotManager = Bootstrap_->GetTabletSlotManager();

        for (const auto& tabletId : tabletIds) {
            const auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tabletId);

            auto* protoTabletInfo = response->add_tablet_info();
            ToProto(protoTabletInfo->mutable_tablet_id(), tabletId);

            for (const auto& replicaPair : tabletSnapshot->Replicas) {
                const auto& replicaId = replicaPair.first;
                const auto& replicaSnapshot = replicaPair.second;

                auto lastReplicationTimestamp =
                    replicaSnapshot->RuntimeData->LastReplicationTimestamp.load(std::memory_order_relaxed);
                if (lastReplicationTimestamp == NullTimestamp) {
                    auto replicationTimestamp =
                        replicaSnapshot->RuntimeData->CurrentReplicationTimestamp.load(std::memory_order_relaxed);
                    lastReplicationTimestamp = replicationTimestamp;
                }

                auto* protoReplicaInfo = protoTabletInfo->add_replica_info();
                ToProto(protoReplicaInfo->mutable_replica_id(), replicaId);
                protoReplicaInfo->set_last_replication_timestamp(lastReplicationTimestamp);
            }
        }
        context->Reply();
    }
};

IServicePtr CreateQueryService(
    TQueryAgentConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
{
    return New<TQueryService>(config, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryAgent
} // namespace NYT

