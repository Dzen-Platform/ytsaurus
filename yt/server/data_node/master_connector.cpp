#include "master_connector.h"
#include "private.h"
#include "artifact.h"
#include "chunk_block_manager.h"
#include "chunk.h"
#include "chunk_cache.h"
#include "chunk_store.h"
#include "config.h"
#include "location.h"
#include "session_manager.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/chunk_server/public.h>

#include <yt/server/data_node/journal_dispatcher.h>

#include <yt/server/job_agent/job_controller.h>

#include <yt/server/misc/memory_usage_tracker.h>

#include <yt/server/tablet_node/slot_manager.h>
#include <yt/server/tablet_node/tablet.h>
#include <yt/server/tablet_node/tablet_slot.h>

#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/election/config.h>

#include <yt/ytlib/hive/cell_directory.h>

#include <yt/ytlib/node_tracker_client/helpers.h>
#include <yt/ytlib/node_tracker_client/node_statistics.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/api/client.h>

#include <yt/ytlib/chunk_client/medium_directory.h>

#include <yt/core/concurrency/delayed_executor.h>

#include <yt/core/misc/serialize.h>
#include <yt/core/misc/string.h>

#include <yt/core/rpc/client.h>

#include <yt/core/ytree/convert.h>

#include <util/random/random.h>

namespace NYT {
namespace NDataNode {

using namespace NYTree;
using namespace NElection;
using namespace NRpc;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient;
using namespace NJobTrackerClient::NProto;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTabletClient::NProto;
using namespace NTabletNode;
using namespace NHydra;
using namespace NHiveClient;
using namespace NObjectClient;
using namespace NTransactionClient;
using namespace NApi;
using namespace NCellNode;

using NNodeTrackerClient::TAddressMap;
using NNodeTrackerClient::TNodeDescriptor;
using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TMasterConnector::TMasterConnector(
    TDataNodeConfigPtr config,
    const TAddressMap& localAddresses,
    const std::vector<Stroka>& nodeTags,
    TBootstrap* bootstrap)
    : Config_(config)
    , LocalAddresses_(localAddresses)
    , NodeTags_(nodeTags)
    , Bootstrap_(bootstrap)
    , ControlInvoker_(bootstrap->GetControlInvoker())
    , LocalDescriptor_(LocalAddresses_)
{
    VERIFY_INVOKER_THREAD_AFFINITY(ControlInvoker_, ControlThread);
    YCHECK(Config_);
    YCHECK(Bootstrap_);
}

void TMasterConnector::Start()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(!Started_);

    Started_ = true;

    auto initializeCell = [&] (TCellTag cellTag) {
        MasterCellTags_.push_back(cellTag);
        YCHECK(ChunksDeltaMap_.insert(std::make_pair(cellTag, TChunksDelta())).second);
    };
    auto connection = Bootstrap_->GetMasterClient()->GetNativeConnection();
    initializeCell(connection->GetPrimaryMasterCellTag());
    for (auto cellTag : connection->GetSecondaryMasterCellTags()) {
        initializeCell(cellTag);
    }

    Bootstrap_->GetChunkStore()->SubscribeChunkAdded(
        BIND(&TMasterConnector::OnChunkAdded, MakeWeak(this))
            .Via(ControlInvoker_));
    Bootstrap_->GetChunkStore()->SubscribeChunkRemoved(
        BIND(&TMasterConnector::OnChunkRemoved, MakeWeak(this))
            .Via(ControlInvoker_));

    Bootstrap_->GetChunkCache()->SubscribeChunkAdded(
        BIND(&TMasterConnector::OnChunkAdded, MakeWeak(this))
            .Via(ControlInvoker_));
    Bootstrap_->GetChunkCache()->SubscribeChunkRemoved(
        BIND(&TMasterConnector::OnChunkRemoved, MakeWeak(this))
            .Via(ControlInvoker_));

    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::StartHeartbeats, MakeStrong(this))
            .Via(ControlInvoker_),
        RandomDuration(Config_->IncrementalHeartbeatPeriod));
}

void TMasterConnector::ForceRegisterAtMaster()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!Started_)
        return;

    ControlInvoker_->Invoke(
        BIND(&TMasterConnector::StartHeartbeats, MakeStrong(this)));
}

void TMasterConnector::StartHeartbeats()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    Reset();

    HeartbeatInvoker_->Invoke(
        BIND(&TMasterConnector::RegisterAtMaster, MakeStrong(this)));
}

bool TMasterConnector::IsConnected() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return NodeId_ != InvalidNodeId;
}

TNodeId TMasterConnector::GetNodeId() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return NodeId_;
}

void TMasterConnector::RegisterAlert(const TError& alert)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YCHECK(!alert.IsOK());

    LOG_WARNING(alert, "Static alert registered");

    {
        TGuard<TSpinLock> guard(AlertsLock_);
        StaticAlerts_.push_back(alert);
    }
}

std::vector<TError> TMasterConnector::GetAlerts()
{
    VERIFY_THREAD_AFFINITY_ANY();

    std::vector<TError> alerts;
    PopulateAlerts_.Fire(&alerts);

    for (const auto& alert : alerts) {
        LOG_WARNING(alert, "Dynamic alert registered");
    }

    {
        TGuard<TSpinLock> guard(AlertsLock_);
        alerts.insert(alerts.end(), StaticAlerts_.begin(), StaticAlerts_.end());
    }

    return alerts;
}

const TAddressMap& TMasterConnector::GetLocalAddresses() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return LocalAddresses_;
}

TNodeDescriptor TMasterConnector::GetLocalDescriptor() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(LocalDescriptorLock_);
    return LocalDescriptor_;
}

void TMasterConnector::ScheduleNodeHeartbeat(TCellTag cellTag, bool immedately)
{
    auto period = immedately
        ? TDuration::Zero()
        : Config_->IncrementalHeartbeatPeriod;
    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::ReportNodeHeartbeat, MakeStrong(this), cellTag)
            .Via(HeartbeatInvoker_),
        period);
}

void TMasterConnector::ScheduleJobHeartbeat(bool immediately)
{
    // NB: Job heartbeats are sent in round-robin fashion,
    // adjust the period accordingly. Also handle #immediately flag.
    auto period = immediately
        ? TDuration::Zero()
        : Config_->IncrementalHeartbeatPeriod /
            (1 + Bootstrap_->GetMasterClient()->GetNativeConnection()->GetSecondaryMasterCellTags().size());
    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::ReportJobHeartbeat, MakeStrong(this))
            .Via(HeartbeatInvoker_),
        period);
}

void TMasterConnector::ResetAndScheduleRegisterAtMaster()
{
    Reset();

    TDelayedExecutor::Submit(
        BIND(&TMasterConnector::RegisterAtMaster, MakeStrong(this))
            .Via(HeartbeatInvoker_),
        Config_->RegisterRetryPeriod);
}

void TMasterConnector::RegisterAtMaster()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    TTransactionStartOptions options;
    options.PingPeriod = Config_->LeaseTransactionPingPeriod;
    options.Timeout = Config_->LeaseTransactionTimeout;

    auto attributes = CreateEphemeralAttributes();
    attributes->Set("title", Format("Lease for node %v", GetDefaultAddress(LocalAddresses_)));
    options.Attributes = std::move(attributes);

    auto asyncTransaction = Bootstrap_->GetMasterClient()->StartTransaction(ETransactionType::Master, options);
    auto transactionOrError = WaitFor(asyncTransaction);

    if (!transactionOrError.IsOK()) {
        LOG_ERROR(transactionOrError, "Error starting lease transaction at primary master");
        ResetAndScheduleRegisterAtMaster();
        return;
    }

    LeaseTransaction_ = transactionOrError.Value();
    LeaseTransaction_->SubscribeAborted(
        BIND(&TMasterConnector::OnLeaseTransactionAborted, MakeWeak(this))
            .Via(HeartbeatInvoker_));

    auto masterChannel = GetMasterChannel(PrimaryMasterCellTag);
    TNodeTrackerServiceProxy proxy(masterChannel);

    auto req = proxy.RegisterNode();
    req->SetTimeout(Config_->RegisterTimeout);
    ComputeTotalStatistics(req->mutable_statistics());
    ToProto(req->mutable_addresses(), LocalAddresses_);
    ToProto(req->mutable_lease_transaction_id(), LeaseTransaction_->GetId());
    ToProto(req->mutable_tags(), NodeTags_);

    LOG_INFO("Node register request sent to primary master (%v)",
        *req->mutable_statistics());

    auto rspOrError = WaitFor(req->Invoke());

    if (!rspOrError.IsOK()) {
        LOG_WARNING(rspOrError, "Error registering node at primary master");
        ResetAndScheduleRegisterAtMaster();
        return;
    }

    const auto& rsp = rspOrError.Value();
    NodeId_ = rsp->node_id();

    try {
        InitMediumDescriptors();
    } catch (const std::exception& ex) {
        LOG_WARNING(TError(ex));
        ResetAndScheduleRegisterAtMaster();
        return;
    }

    for (auto cellTag : MasterCellTags_) {
        auto* delta = GetChunksDelta(cellTag);
        delta->State = EState::Registered;
    }

    MediumUpdateExecutor_ = New<TPeriodicExecutor>(
        HeartbeatInvoker_,
        BIND(&TMasterConnector::OnMediumDescriptorsUpdate, MakeWeak(this)),
        Bootstrap_->GetMasterClient()->GetNativeConnection()->GetConfig()->MediumDirectorySynchronizer->SyncPeriod);
    MediumUpdateExecutor_->Start();

    MasterConnected_.Fire();

    LOG_INFO("Successfully registered at primary master (NodeId: %v)",
        NodeId_);

    for (auto cellTag : MasterCellTags_) {
        ScheduleNodeHeartbeat(cellTag, true);
    }
    ScheduleJobHeartbeat(true);
}

void TMasterConnector::InitMediumDescriptors()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Initializing medium descriptors");

    const auto& client = Bootstrap_->GetMasterClient();
    const auto& connection = client->GetNativeConnection();
    WaitFor(connection->SynchronizeMediumDirectory())
        .ThrowOnError();

    DoUpdateMediumDescriptors();

    LOG_INFO("Medium descriptors initialized");
}

void TMasterConnector::OnMediumDescriptorsUpdate()
{
    try {
        DoUpdateMediumDescriptors();
    } catch (const std::exception& ex) {
        LOG_WARNING(ex, "Error updating medium descriptors");
    }
}

void TMasterConnector::DoUpdateMediumDescriptors()
{
    const auto& client = Bootstrap_->GetMasterClient();
    const auto& connection = client->GetNativeConnection();
    const auto& mediumDirectory = connection->GetMediumDirectory();

    auto updateLocation = [&] (const TLocationPtr& location) {
        const auto& oldDescriptor = location->GetMediumDescriptor();
        const auto* newDescriptor = mediumDirectory->FindByName(location->GetMediumName());
        if (!newDescriptor) {
            THROW_ERROR_EXCEPTION("Location %Qv refers to unknown medium %Qv",
                location->GetId(),
                location->GetMediumName());
        }
        if (oldDescriptor.Index != InvalidMediumIndex &&
            oldDescriptor.Index != newDescriptor->Index)
        {
            THROW_ERROR_EXCEPTION("Medium %Qv has changed its index from %v to %v",
                location->GetMediumName(),
                oldDescriptor.Index,
                newDescriptor->Index);
        }
        location->SetMediumDescriptor(*newDescriptor);
    };

    for (const auto& location : Bootstrap_->GetChunkStore()->Locations()) {
        updateLocation(location);
    }
    for (const auto& location : Bootstrap_->GetChunkCache()->Locations()) {
        updateLocation(location);
    }
}

void TMasterConnector::OnLeaseTransactionAborted()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_WARNING("Master transaction lease aborted");

    ResetAndScheduleRegisterAtMaster();
}

TNodeStatistics TMasterConnector::ComputeStatistics()
{
    TNodeStatistics result;
    ComputeTotalStatistics(&result);
    ComputeLocationSpecificStatistics(&result);
    return result;
}

void TMasterConnector::ComputeTotalStatistics(TNodeStatistics* result)
{
    auto chunkStore = Bootstrap_->GetChunkStore();

    i64 totalAvailableSpace = 0;
    i64 totalLowWatermarkSpace = 0;
    i64 totalUsedSpace = 0;
    int totalStoredChunkCount = 0;
    int totalSessionCount = 0;
    bool full = true;

    for (auto location : chunkStore->Locations()) {
        if (location->IsEnabled()) {
            totalAvailableSpace += location->GetAvailableSpace();
            totalLowWatermarkSpace += location->GetLowWatermarkSpace();
            full &= location->IsFull();
        }

        totalUsedSpace += location->GetUsedSpace();
        totalStoredChunkCount += location->GetChunkCount();
        totalSessionCount += location->GetSessionCount();
    }

    // Do not treat node without locations as empty; motivating case is the following:
    // when extending cluster with cloud-nodes for more computational resources,
    // we do not want to replicate data on those cloud-nodes (thus to enable locations
    // on those nodes) because they can go offline all at once. Hence we are
    // not counting these cloud-nodes as full.
    if (chunkStore->Locations().empty()) {
        full = false;
    }

    auto chunkCache = Bootstrap_->GetChunkCache();
    int totalCachedChunkCount = chunkCache->GetChunkCount();

    result->set_total_available_space(totalAvailableSpace);
    result->set_total_low_watermark_space(totalLowWatermarkSpace);
    result->set_total_used_space(totalUsedSpace);
    result->set_total_stored_chunk_count(totalStoredChunkCount);
    result->set_total_cached_chunk_count(totalCachedChunkCount);
    result->set_full(full);

    auto sessionManager = Bootstrap_->GetSessionManager();
    result->set_total_user_session_count(sessionManager->GetSessionCount(ESessionType::User));
    result->set_total_replication_session_count(sessionManager->GetSessionCount(ESessionType::Replication));
    result->set_total_repair_session_count(sessionManager->GetSessionCount(ESessionType::Repair));

    auto slotManager = Bootstrap_->GetTabletSlotManager();
    result->set_available_tablet_slots(slotManager->GetAvailableTabletSlotCount());
    result->set_used_tablet_slots(slotManager->GetUsedTableSlotCount());

    const auto* tracker = Bootstrap_->GetMemoryUsageTracker();
    auto* protoMemory = result->mutable_memory();
    protoMemory->set_total_limit(tracker->GetTotalLimit());
    protoMemory->set_total_used(tracker->GetTotalUsed());
    for (auto category : TEnumTraits<EMemoryCategory>::GetDomainValues()) {
        auto* protoCategory = protoMemory->add_categories();
        protoCategory->set_type(static_cast<int>(category));
        auto limit = tracker->GetLimit(category);
        if (limit < std::numeric_limits<i64>::max()) {
            protoCategory->set_limit(limit);
        }
        auto used = tracker->GetUsed(category);
        protoCategory->set_used(used);
    }
}

void TMasterConnector::ComputeLocationSpecificStatistics(TNodeStatistics* result)
{
    const auto& chunkStore = Bootstrap_->GetChunkStore();

    struct TMediumStatistics
    {
        double IOWeight = 0.0;
    };

    yhash<int, TMediumStatistics> mediaStatistics;

    for (const auto& location : chunkStore->Locations()) {
        auto* locationStatistics = result->add_locations();

        auto mediumIndex = location->GetMediumDescriptor().Index;
        locationStatistics->set_medium_index(mediumIndex);
        locationStatistics->set_available_space(location->GetAvailableSpace());
        locationStatistics->set_used_space(location->GetUsedSpace());
        locationStatistics->set_low_watermark_space(location->GetLowWatermarkSpace());
        locationStatistics->set_chunk_count(location->GetChunkCount());
        locationStatistics->set_session_count(location->GetSessionCount());
        locationStatistics->set_enabled(location->IsEnabled());
        locationStatistics->set_full(location->IsFull());

        auto& mediumStatistics = mediaStatistics[mediumIndex];
        if (location->IsEnabled() && !location->IsFull()) {
            ++mediumStatistics.IOWeight;
        }
    }

    for (const auto& pair : mediaStatistics) {
        int mediumIndex = pair.first;
        const auto& mediumStatisitcs = pair.second;
        auto* protoStatistics = result->add_media();
        protoStatistics->set_medium_index(mediumIndex);
        protoStatistics->set_io_weight(mediumStatisitcs.IOWeight);
    }
}

void TMasterConnector::ReportNodeHeartbeat(TCellTag cellTag)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto* delta = GetChunksDelta(cellTag);
    switch (delta->State) {
        case EState::Registered:
            if (CanSendFullNodeHeartbeat(cellTag)) {
                ReportFullNodeHeartbeat(cellTag);
            } else {
                ScheduleNodeHeartbeat(cellTag);
            }
            break;

        case EState::Online:
            ReportIncrementalNodeHeartbeat(cellTag);
            break;

        default:
            Y_UNREACHABLE();
    }
}

bool TMasterConnector::CanSendFullNodeHeartbeat(TCellTag cellTag)
{
    auto connection = Bootstrap_->GetMasterClient()->GetNativeConnection();
    if (cellTag != connection->GetPrimaryMasterCellTag()) {
        return true;
    }

    for (const auto& pair : ChunksDeltaMap_) {
        auto cellTag = pair.first;
        const auto& delta = pair.second;
        if (cellTag != connection->GetPrimaryMasterCellTag() && delta.State != EMasterConnectorState::Online) {
            return false;
        }
    }
    return true;
}

void TMasterConnector::ReportFullNodeHeartbeat(TCellTag cellTag)
{
    auto Logger = DataNodeLogger;
    Logger.AddTag("CellTag: %v", cellTag);

    auto channel = GetMasterChannel(cellTag);
    TNodeTrackerServiceProxy proxy(channel);

    auto request = proxy.FullHeartbeat();
    request->SetCodec(NCompression::ECodec::Lz4);
    request->SetTimeout(Config_->FullHeartbeatTimeout);

    YCHECK(NodeId_ != InvalidNodeId);
    request->set_node_id(NodeId_);

    *request->mutable_statistics() = ComputeStatistics();

    NChunkServer::TPerMediumIntArray chunkCounts{};

    int storedChunkCount = 0;
    int cachedChunkCount = 0;

    auto addStoredChunkInfo = [&] (const IChunkPtr& chunk) {
        if (CellTagFromId(chunk->GetId()) == cellTag) {
            auto info = BuildAddChunkInfo(chunk);
            *request->add_chunks() = info;
            auto mediumIndex = chunk->GetLocation()->GetMediumDescriptor().Index;
            ++chunkCounts[mediumIndex];
            ++storedChunkCount;
        }
    };

    auto addCachedChunkInfo = [&] (const IChunkPtr& chunk) {
        if (!IsArtifactChunkId(chunk->GetId())) {
            auto info = BuildAddChunkInfo(chunk);
            *request->add_chunks() = info;
            ++chunkCounts[DefaultCacheMediumIndex];
            ++cachedChunkCount;
        }
    };

    for (const auto& chunk : Bootstrap_->GetChunkStore()->GetChunks()) {
        addStoredChunkInfo(chunk);
    }

    for (const auto& chunk : Bootstrap_->GetChunkCache()->GetChunks()) {
        addCachedChunkInfo(chunk);
    }

    int mediumIndex = 0;
    for (auto chunkCount : chunkCounts) {
        if (chunkCount != 0) {
            auto* mediumChunkStatistics = request->add_chunk_statistics();
            mediumChunkStatistics->set_medium_index(mediumIndex);
            mediumChunkStatistics->set_chunk_count(chunkCount);
        }

        ++mediumIndex;
    }

    LOG_INFO("Full node heartbeat sent to master (StoredChunkCount: %v, CachedChunkCount: %v, %v)",
        storedChunkCount,
        cachedChunkCount,
        request->statistics());

    auto rspOrError = WaitFor(request->Invoke());

    if (!rspOrError.IsOK()) {
        LOG_WARNING(rspOrError, "Error reporting full node heartbeat to master",
            cellTag);

        if (NRpc::IsRetriableError(rspOrError)) {
            ScheduleNodeHeartbeat(cellTag);
        } else {
            ResetAndScheduleRegisterAtMaster();
        }
        return;
    }

    LOG_INFO("Successfully reported full node heartbeat to master");

    // Schedule another full heartbeat.
    if (Config_->FullHeartbeatPeriod) {
        TDelayedExecutor::Submit(
            BIND(&TMasterConnector::StartHeartbeats, MakeStrong(this))
                .Via(HeartbeatInvoker_),
            RandomDuration(*Config_->FullHeartbeatPeriod));
    }

    auto* delta = GetChunksDelta(cellTag);
    delta->State = EState::Online;
    YCHECK(delta->AddedSinceLastSuccess.empty());
    YCHECK(delta->RemovedSinceLastSuccess.empty());

    ScheduleNodeHeartbeat(cellTag);
}

TFuture<void> TMasterConnector::GetHeartbeatBarrier(TCellTag cellTag)
{
    return GetChunksDelta(cellTag)->HeartbeatBarrier;
}

void TMasterConnector::ReportIncrementalNodeHeartbeat(TCellTag cellTag)
{
    auto Logger = DataNodeLogger;
    Logger.AddTag("CellTag: %v", cellTag);

    auto primaryCellTag = CellTagFromId(Bootstrap_->GetCellId());

    auto channel = GetMasterChannel(cellTag);
    TNodeTrackerServiceProxy proxy(channel);

    auto request = proxy.IncrementalHeartbeat();
    request->SetCodec(NCompression::ECodec::Lz4);
    request->SetTimeout(Config_->IncrementalHeartbeatTimeout);

    YCHECK(NodeId_ != InvalidNodeId);
    request->set_node_id(NodeId_);

    *request->mutable_statistics() = ComputeStatistics();

    ToProto(request->mutable_alerts(), GetAlerts());

    auto* delta = GetChunksDelta(cellTag);

    auto barrierPromise = std::move(delta->HeartbeatBarrier);
    delta->HeartbeatBarrier = NewPromise<void>();

    delta->ReportedAdded.clear();
    for (const auto& chunk : delta->AddedSinceLastSuccess) {
        YCHECK(delta->ReportedAdded.insert(std::make_pair(chunk, chunk->GetVersion())).second);
        *request->add_added_chunks() = BuildAddChunkInfo(chunk);
    }

    delta->ReportedRemoved.clear();
    for (const auto& chunk : delta->RemovedSinceLastSuccess) {
        YCHECK(delta->ReportedRemoved.insert(chunk).second);
        *request->add_removed_chunks() = BuildRemoveChunkInfo(chunk);
    }

    if (cellTag == primaryCellTag) {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        for (auto slot : slotManager->Slots()) {
            auto* protoSlotInfo = request->add_tablet_slots();
            if (slot) {
                ToProto(protoSlotInfo->mutable_cell_info(), slot->GetCellDescriptor().ToInfo());
                protoSlotInfo->set_peer_state(static_cast<int>(slot->GetControlState()));
                protoSlotInfo->set_peer_id(slot->GetPeerId());
            } else {
                protoSlotInfo->set_peer_state(static_cast<int>(NHydra::EPeerState::None));
            }
        }

        auto tabletSnapshots = slotManager->GetTabletSnapshots();
        for (const auto& tabletSnapshot : tabletSnapshots) {
            auto* protoTabletInfo = request->add_tablets();
            ToProto(protoTabletInfo->mutable_tablet_id(), tabletSnapshot->TabletId);

            auto* protoTabletStatistics = protoTabletInfo->mutable_statistics();
            protoTabletStatistics->set_partition_count(tabletSnapshot->PartitionList.size());
            protoTabletStatistics->set_store_count(tabletSnapshot->StoreCount);
            protoTabletStatistics->set_preload_pending_store_count(tabletSnapshot->PreloadPendingStoreCount);
            protoTabletStatistics->set_preload_completed_store_count(tabletSnapshot->PreloadCompletedStoreCount);
            protoTabletStatistics->set_preload_failed_store_count(tabletSnapshot->PreloadFailedStoreCount);
            protoTabletStatistics->set_overlapping_store_count(tabletSnapshot->OverlappingStoreCount);
            protoTabletStatistics->set_last_commit_timestamp(tabletSnapshot->RuntimeData->LastCommitTimestamp);
            protoTabletStatistics->set_unflushed_timestamp(tabletSnapshot->RuntimeData->UnflushedTimestamp);

            for (const auto& pair : tabletSnapshot->Replicas) {
                const auto& replicaId = pair.first;
                const auto& replicaSnapshot = pair.second;
                auto* protoReplicaInfo = protoTabletInfo->add_replicas();
                ToProto(protoReplicaInfo->mutable_replica_id(), replicaId);
                replicaSnapshot->RuntimeData->Populate(protoReplicaInfo->mutable_statistics());
            }

            auto* protoPerformanceCounters = protoTabletInfo->mutable_performance_counters();
            auto performanceCounters = tabletSnapshot->PerformanceCounters;
            protoPerformanceCounters->set_dynamic_row_read_count(performanceCounters->DynamicRowReadCount);
            protoPerformanceCounters->set_dynamic_row_lookup_count(performanceCounters->DynamicRowLookupCount);
            protoPerformanceCounters->set_dynamic_row_write_count(performanceCounters->DynamicRowWriteCount);
            protoPerformanceCounters->set_dynamic_row_delete_count(performanceCounters->DynamicRowDeleteCount);
            protoPerformanceCounters->set_static_chunk_row_read_count(performanceCounters->StaticChunkRowReadCount);
            protoPerformanceCounters->set_static_chunk_row_lookup_count(performanceCounters->StaticChunkRowLookupCount);
            protoPerformanceCounters->set_static_chunk_row_lookup_true_negative_count(performanceCounters->StaticChunkRowLookupTrueNegativeCount);
            protoPerformanceCounters->set_static_chunk_row_lookup_false_positive_count(performanceCounters->StaticChunkRowLookupFalsePositiveCount);
            protoPerformanceCounters->set_unmerged_row_read_count(performanceCounters->UnmergedRowReadCount);
            protoPerformanceCounters->set_merged_row_read_count(performanceCounters->MergedRowReadCount);
        }
    }

    LOG_INFO("Incremental node heartbeat sent to master (%v, AddedChunks: %v, RemovedChunks: %v)",
        request->statistics(),
        request->added_chunks_size(),
        request->removed_chunks_size());

    auto rspOrError = WaitFor(request->Invoke());
    if (!rspOrError.IsOK()) {
        delta->HeartbeatBarrier.SetFrom(barrierPromise.ToFuture());
        delta->HeartbeatBarrier = std::move(barrierPromise);

        LOG_WARNING(rspOrError, "Error reporting incremental node heartbeat to master");
        if (NRpc::IsRetriableError(rspOrError)) {
            ScheduleNodeHeartbeat(cellTag);
        } else {
            ResetAndScheduleRegisterAtMaster();
        }
        return;
    }

    LOG_INFO("Successfully reported incremental node heartbeat to master");

    barrierPromise.Set();

    const auto& rsp = rspOrError.Value();

    {
        auto it = delta->AddedSinceLastSuccess.begin();
        while (it != delta->AddedSinceLastSuccess.end()) {
            auto jt = it++;
            auto chunk = *jt;
            auto kt = delta->ReportedAdded.find(chunk);
            if (kt != delta->ReportedAdded.end() && kt->second == chunk->GetVersion()) {
                delta->AddedSinceLastSuccess.erase(jt);
            }
        }
        delta->ReportedAdded.clear();
    }

    {
        auto it = delta->RemovedSinceLastSuccess.begin();
        while (it != delta->RemovedSinceLastSuccess.end()) {
            auto jt = it++;
            auto chunk = *jt;
            auto kt = delta->ReportedRemoved.find(chunk);
            if (kt != delta->ReportedRemoved.end()) {
                delta->RemovedSinceLastSuccess.erase(jt);
            }
        }
        delta->ReportedRemoved.clear();
    }

    if (cellTag == primaryCellTag) {
        auto rack = rsp->has_rack() ? MakeNullable(rsp->rack()) : Null;
        UpdateRack(rack);

        auto dc = rsp->has_data_center() ? MakeNullable(rsp->data_center()) : Null;
        UpdateDataCenter(dc);

        auto jobController = Bootstrap_->GetJobController();
        jobController->SetResourceLimitsOverrides(rsp->resource_limits_overrides());
        jobController->SetDisableSchedulerJobs(rsp->disable_scheduler_jobs());

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        for (const auto& info : rsp->tablet_slots_to_remove()) {
            auto cellId = FromProto<TCellId>(info.cell_id());
            YCHECK(cellId);
            auto slot = slotManager->FindSlot(cellId);
            if (!slot) {
                LOG_WARNING("Requested to remove a non-existing slot %v, ignored",
                    cellId);
                continue;
            }
            slotManager->RemoveSlot(slot);
        }

        for (const auto& info : rsp->tablet_slots_to_create()) {
            auto cellId = FromProto<TCellId>(info.cell_id());
            YCHECK(cellId);
            if (slotManager->GetAvailableTabletSlotCount() == 0) {
                LOG_WARNING("Requested to start cell %v when all slots are used, ignored",
                    cellId);
                continue;
            }
            if (slotManager->FindSlot(cellId)) {
                LOG_WARNING("Requested to start cell %v when this cell is already being served by the node, ignored",
                    cellId);
                continue;
            }
            slotManager->CreateSlot(info);
        }

        for (const auto& info : rsp->tablet_slots_configure()) {
            auto descriptor = FromProto<TCellDescriptor>(info.cell_descriptor());
            auto slot = slotManager->FindSlot(descriptor.CellId);
            if (!slot) {
                LOG_WARNING("Requested to configure a non-existing slot %v, ignored",
                    descriptor.CellId);
                continue;
            }
            if (!slot->CanConfigure()) {
                LOG_WARNING("Cannot configure slot %v in state %Qlv, ignored",
                    descriptor.CellId,
                    slot->GetControlState());
                continue;
            }
            slotManager->ConfigureSlot(slot, info);
        }
    }

    ScheduleNodeHeartbeat(cellTag);
}

TChunkAddInfo TMasterConnector::BuildAddChunkInfo(IChunkPtr chunk)
{
    TChunkAddInfo result;
    ToProto(result.mutable_chunk_id(), chunk->GetId());
    result.set_medium_index(chunk->GetLocation()->GetMediumDescriptor().Index);
    result.set_active(chunk->IsActive());
    result.set_sealed(chunk->GetInfo().sealed());
    return result;
}

TChunkRemoveInfo TMasterConnector::BuildRemoveChunkInfo(IChunkPtr chunk)
{
    TChunkRemoveInfo result;
    ToProto(result.mutable_chunk_id(), chunk->GetId());
    result.set_medium_index(chunk->GetLocation()->GetMediumDescriptor().Index);
    return result;
}

void TMasterConnector::ReportJobHeartbeat()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(NodeId_ != InvalidNodeId);

    auto cellTag = MasterCellTags_[JobHeartbeatCellIndex_];
    auto Logger = DataNodeLogger;
    Logger.AddTag("CellTag: %v", cellTag);

    auto* delta = GetChunksDelta(cellTag);
    if (delta->State == EState::Online) {
        auto channel = GetMasterChannel(cellTag);
        TJobTrackerServiceProxy proxy(channel);

        auto req = proxy.Heartbeat();

        auto jobController = Bootstrap_->GetJobController();
        jobController->PrepareHeartbeatRequest(
            cellTag,
            EObjectType::MasterJob,
            req.Get());

        LOG_INFO("Job heartbeat sent to master (ResourceUsage: %v)",
            FormatResourceUsage(req->resource_usage(), req->resource_limits()));

        auto rspOrError = WaitFor(req->Invoke());

        if (!rspOrError.IsOK()) {
            LOG_WARNING(rspOrError, "Error reporting job heartbeat to master");
            if (NRpc::IsRetriableError(rspOrError)) {
                ScheduleJobHeartbeat();
            } else {
                ResetAndScheduleRegisterAtMaster();
            }
            return;
        }

        LOG_INFO("Successfully reported job heartbeat to master");

        const auto& rsp = rspOrError.Value();
        jobController->ProcessHeartbeatResponse(rsp);
    }

    if (++JobHeartbeatCellIndex_ >= MasterCellTags_.size()) {
        JobHeartbeatCellIndex_ = 0;
    }

    ScheduleJobHeartbeat();
}

void TMasterConnector::Reset()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (HeartbeatContext_) {
        HeartbeatContext_->Cancel();
    }

    HeartbeatContext_ = New<TCancelableContext>();
    HeartbeatInvoker_ = HeartbeatContext_->CreateInvoker(ControlInvoker_);

    NodeId_ = InvalidNodeId;
    JobHeartbeatCellIndex_ = 0;
    LeaseTransaction_.Reset();

    for (auto cellTag : MasterCellTags_) {
        auto* delta = GetChunksDelta(cellTag);
        delta->State = EState::Offline;
        delta->ReportedAdded.clear();
        delta->ReportedRemoved.clear();
        delta->AddedSinceLastSuccess.clear();
        delta->RemovedSinceLastSuccess.clear();
    }

    if (MediumUpdateExecutor_) {
        MediumUpdateExecutor_->Stop();
        MediumUpdateExecutor_.Reset();
    }

    MasterDisconnected_.Fire();

    LOG_INFO("Master disconnected");
}

void TMasterConnector::OnChunkAdded(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (IsArtifactChunkId(chunk->GetId()))
        return;

    auto* delta = GetChunksDelta(chunk->GetId());
    if (delta->State != EState::Online)
        return;

    delta->RemovedSinceLastSuccess.erase(chunk);
    delta->AddedSinceLastSuccess.insert(chunk);

    LOG_DEBUG("Chunk addition registered (ChunkId: %v, LocationId: %v)",
        chunk->GetId(),
        chunk->GetLocation()->GetId());
}

void TMasterConnector::OnChunkRemoved(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (IsArtifactChunkId(chunk->GetId()))
        return;

    auto* delta = GetChunksDelta(chunk->GetId());
    if (delta->State != EState::Online)
        return;

    delta->AddedSinceLastSuccess.erase(chunk);
    delta->RemovedSinceLastSuccess.insert(chunk);

    LOG_DEBUG("Chunk removal registered (ChunkId: %v, LocationId: %v)",
        chunk->GetId(),
        chunk->GetLocation()->GetId());
}

IChannelPtr TMasterConnector::GetMasterChannel(TCellTag cellTag)
{
    auto cellId = Bootstrap_->GetCellId(cellTag);
    auto client = Bootstrap_->GetMasterClient();
    auto connection = client->GetNativeConnection();
    auto cellDirectory = connection->GetCellDirectory();
    return cellDirectory->GetChannel(cellId, EPeerKind::Leader);
}

void TMasterConnector::UpdateRack(const TNullable<Stroka>& rack)
{
    TGuard<TSpinLock> guard(LocalDescriptorLock_);
    LocalDescriptor_ = TNodeDescriptor(
        LocalAddresses_,
        rack,
        LocalDescriptor_.GetDataCenter());
}

void TMasterConnector::UpdateDataCenter(const TNullable<Stroka>& dc)
{
    TGuard<TSpinLock> guard(LocalDescriptorLock_);
    LocalDescriptor_ = TNodeDescriptor(
        LocalAddresses_,
        LocalDescriptor_.GetRack(),
        dc);
}

TMasterConnector::TChunksDelta* TMasterConnector::GetChunksDelta(TCellTag cellTag)
{
    auto it = ChunksDeltaMap_.find(cellTag);
    Y_ASSERT(it != ChunksDeltaMap_.end());
    return &it->second;
}

TMasterConnector::TChunksDelta* TMasterConnector::GetChunksDelta(const TObjectId& id)
{
    return GetChunksDelta(CellTagFromId(id));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
