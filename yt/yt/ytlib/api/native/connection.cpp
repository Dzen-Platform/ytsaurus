#include "config.h"
#include "connection.h"
#include "client.h"
#include "sync_replica_cache.h"
#include "tablet_sync_replica_cache.h"
#include "transaction_participant.h"
#include "transaction.h"
#include "private.h"
#include "helpers.h"

#include <yt/yt/ytlib/auth/native_authentication_manager.h>
#include <yt/yt/ytlib/auth/native_authenticating_channel.h>

#include <yt/yt/ytlib/chaos_client/banned_replica_tracker.h>
#include <yt/yt/ytlib/chaos_client/chaos_cell_directory_synchronizer.h>
#include <yt/yt/ytlib/chaos_client/native_replication_card_cache_detail.h>
#include <yt/yt/ytlib/chaos_client/replication_card_channel_factory.h>
#include <yt/yt/ytlib/chaos_client/replication_card_residency_cache.h>

#include <yt/yt/ytlib/cell_master_client/cell_directory.h>
#include <yt/yt/ytlib/cell_master_client/cell_directory_synchronizer.h>

#include <yt/yt/ytlib/chunk_client/chunk_meta_cache.h>
#include <yt/yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/yt/ytlib/chunk_client/medium_directory.h>
#include <yt/yt/ytlib/chunk_client/medium_directory_synchronizer.h>
#include <yt/yt/ytlib/chunk_client/chunk_replica_cache.h>

#include <yt/yt/ytlib/hive/cell_directory.h>
#include <yt/yt/ytlib/hive/cell_directory_synchronizer.h>
#include <yt/yt/ytlib/hive/cell_tracker.h>
#include <yt/yt/ytlib/hive/cluster_directory.h>
#include <yt/yt/ytlib/hive/cluster_directory_synchronizer.h>
#include <yt/yt/ytlib/hive/hive_service_proxy.h>

#include <yt/yt/ytlib/hydra/peer_channel.h>

#include <yt/yt/library/query/engine/column_evaluator.h>
#include <yt/yt/library/query/engine/evaluator.h>
#include <yt/yt/ytlib/query_client/functions_cache.h>

#include <yt/yt/ytlib/queue_client/config.h>
#include <yt/yt/ytlib/queue_client/registration_manager.h>

#include <yt/yt/ytlib/yql_client/config.h>

#include <yt/yt/ytlib/discovery_client/discovery_client.h>
#include <yt/yt/ytlib/discovery_client/member_client.h>

#include <yt/yt/ytlib/node_tracker_client/node_addresses_provider.h>
#include <yt/yt/ytlib/node_tracker_client/node_directory_synchronizer.h>

#include <yt/yt/ytlib/job_prober_client/job_shell_descriptor_cache.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/ytlib/scheduler/scheduler_channel.h>

#include <yt/yt/ytlib/security_client/permission_cache.h>

#include <yt/yt/ytlib/tablet_client/native_table_mount_cache.h>

#include <yt/yt/ytlib/transaction_client/config.h>
#include <yt/yt/ytlib/transaction_client/clock_manager.h>

#include <yt/yt/client/tablet_client/table_mount_cache.h>

#include <yt/yt/client/api/sticky_transaction_pool.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/transaction_client/config.h>
#include <yt/yt/client/transaction_client/noop_timestamp_provider.h>
#include <yt/yt/client/transaction_client/remote_timestamp_provider.h>

#include <yt/yt/library/auth_server/tvm_service.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/thread_pool.h>
#include <yt/yt/core/concurrency/lease_manager.h>
#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/rpc/bus/channel.h>

#include <yt/yt/core/rpc/caching_channel_factory.h>
#include <yt/yt/core/rpc/balancing_channel.h>
#include <yt/yt/core/rpc/retrying_channel.h>
#include <yt/yt/core/rpc/helpers.h>

#include <yt/yt/core/misc/atomic_object.h>
#include <yt/yt/core/misc/checksum.h>
#include <yt/yt/core/misc/memory_usage_tracker.h>

namespace NYT::NApi::NNative {

using namespace NAuth;
using namespace NChaosClient;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NHiveClient;
using namespace NHydra;
using namespace NJobProberClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NProfiling;
using namespace NQueryClient;
using namespace NQueueClient;
using namespace NRpc;
using namespace NScheduler;
using namespace NScheduler;
using namespace NSecurityClient;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NYson;
using namespace NDiscoveryClient;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

namespace {

TString MakeConnectionClusterId(const TConnectionConfigPtr& config)
{
    if (config->ClusterName) {
        return Format("Native(Name=%v)", *config->ClusterName);
    } else {
        return Format("Native(PrimaryCellTag=%v)",
            CellTagFromId(config->PrimaryMaster->CellId));
    }
}

} // namespace

class TConnection
    : public IConnection
{
public:
    TConnection(
        TConnectionConfigPtr config,
        const TConnectionOptions& options)
        : Config_(std::move(config))
        , Options_(options)
        , LoggingTag_(Format("PrimaryCellTag: %v, ConnectionId: %v, ConnectionName: %v",
            CellTagFromId(Config_->PrimaryMaster->CellId),
            TGuid::Create(),
            Config_->ConnectionName))
        , ClusterId_(MakeConnectionClusterId(Config_))
        , StickyGroupSizeCache_(Config_->EnableDynamicCacheStickyGroupSize ? New<TStickyGroupSizeCache>() : nullptr)
        , Logger(ApiLogger.WithRawTag(LoggingTag_))
        , Profiler_(TProfiler("/connection").WithTag("connection_name", Config_->ConnectionName))
        , TabletSyncReplicaCache_(New<TTabletSyncReplicaCache>())
        , BannedReplicaTrackerCache_(CreateBannedReplicaTrackerCache(Config_->BannedReplicaTrackerCache, Logger))
    {
        ChannelFactory_ = CreateNativeAuthenticationInjectingChannelFactory(
            CreateCachingChannelFactory(
                NRpc::NBus::CreateBusChannelFactory(Config_->BusClient),
                Config_->IdleChannelTtl),
            Config_->TvmId,
            Options_.TvmService);
    }

    void Initialize()
    {
        if (!Options_.ConnectionInvoker) {
            ConnectionThreadPool_ = CreateThreadPool(Config_->ThreadPoolSize, "Connection");
            Options_.ConnectionInvoker = ConnectionThreadPool_->GetInvoker();
        }

        MasterCellDirectory_ = New<NCellMasterClient::TCellDirectory>(
            Config_,
            Options_,
            ChannelFactory_,
            Logger);
        MasterCellDirectorySynchronizer_ = New<NCellMasterClient::TCellDirectorySynchronizer>(
            Config_->MasterCellDirectorySynchronizer,
            MasterCellDirectory_);

        InitializeTimestampProvider();

        ClockManager_ = CreateClockManager(
            Config_->ClockManager,
            this,
            Logger);

        SchedulerChannel_ = CreateSchedulerChannel(
            Config_->Scheduler,
            ChannelFactory_,
            GetMasterChannelOrThrow(EMasterChannelKind::Leader),
            GetNetworks());

        InitializeQueueAgentChannels();
        QueueConsumerRegistrationManager_ = New<TQueueConsumerRegistrationManager>(
            Config_->QueueAgent->QueueConsumerRegistrationManager,
            this,
            GetInvoker(),
            Logger);

        InitializeYqlAgentChannel();

        PermissionCache_ = New<TPermissionCache>(
            Config_->PermissionCache,
            this);

        JobShellDescriptorCache_ = New<TJobShellDescriptorCache>(
            Config_->JobShellDescriptorCache,
            SchedulerChannel_);

        ClusterDirectory_ = New<TClusterDirectory>(Options_);
        ClusterDirectorySynchronizer_ = New<TClusterDirectorySynchronizer>(
            Config_->ClusterDirectorySynchronizer,
            this,
            ClusterDirectory_);

        MediumDirectory_ = New<TMediumDirectory>();
        MediumDirectorySynchronizer_ = New<TMediumDirectorySynchronizer>(
            Config_->MediumDirectorySynchronizer,
            this,
            MediumDirectory_);

        CellDirectory_ = NHiveClient::CreateCellDirectory(
            Config_->CellDirectory,
            ChannelFactory_,
            ClusterDirectory_,
            GetNetworks(),
            Logger);
        ConfigureMasterCells();

        CellDirectorySynchronizer_ = CreateCellDirectorySynchronizer(
            Config_->CellDirectorySynchronizer,
            CellDirectory_,
            GetCellDirectorySynchronizerSourceOfTruthCellIds(),
            Logger);

        ChaosCellDirectorySynchronizer_ = CreateChaosCellDirectorySynchronizer(
            Config_->ChaosCellDirectorySynchronizer,
            CellDirectory_,
            this,
            Logger);

        if (Config_->ReplicationCardCache || Config_->ChaosCellDirectorySynchronizer->SyncAllChaosCells) {
            ChaosCellDirectorySynchronizer_->Start();
        }

        ReplicationCardChannelFactory_ = CreateReplicationCardChannelFactory(
            CellDirectory_,
            CreateReplicationCardResidencyCache(
                Config_->ReplicationCardResidencyCache,
                this,
                Logger),
            ChaosCellDirectorySynchronizer_,
            Config_->ChaosCellChannel);

        if (Options_.BlockCache) {
            BlockCache_ = Options_.BlockCache;
        } else {
            BlockCache_ = CreateClientBlockCache(
                Config_->BlockCache,
                EBlockType::CompressedData | EBlockType::UncompressedData,
                /*memoryTracker*/ nullptr,
                /*blockTracker*/ nullptr,
                Profiler_.WithPrefix("/block_cache"));
        }

        if (Options_.ChunkMetaCache) {
            ChunkMetaCache_ = Options_.ChunkMetaCache;
        } else if (Config_->ChunkMetaCache) {
            ChunkMetaCache_ = CreateClientChunkMetaCache(
                Config_->ChunkMetaCache,
                Profiler_.WithPrefix("/chunk_meta_cache"));
        } else {
            ChunkMetaCache_ = nullptr;
        }

        TableMountCache_ = CreateNativeTableMountCache(
            Config_->TableMountCache,
            this,
            CellDirectory_,
            Logger,
            Profiler_);

        if (const auto& config = Config_->ReplicationCardCache) {
            ReplicationCardCache_ = CreateNativeReplicationCardCache(
                config,
                this,
                Logger);
        }

        QueryEvaluator_ = CreateEvaluator(Config_->QueryEvaluator);
        ColumnEvaluatorCache_ = CreateColumnEvaluatorCache(Config_->ColumnEvaluatorCache);

        SyncReplicaCache_ = New<TSyncReplicaCache>(
            Config_->SyncReplicaCache,
            this,
            Logger);

        NodeDirectory_ = New<TNodeDirectory>();
        NodeDirectorySynchronizer_ = CreateNodeDirectorySynchronizer(
            MakeStrong(this),
            NodeDirectory_);

        ChunkReplicaCache_ = CreateChunkReplicaCache(this);

        SetupTvmIdSynchronization();
    }

    // IConnection implementation.

    TClusterTag GetClusterTag() const override
    {
        return GetPrimaryMasterCellTag();
    }

    const TString& GetLoggingTag() const override
    {
        return LoggingTag_;
    }

    const TString& GetClusterId() const override
    {
        return ClusterId_;
    }

    bool IsSameCluster(const NApi::IConnectionPtr& other) const override
    {
        return GetClusterTag() == other->GetClusterTag();
    }

    const ITableMountCachePtr& GetTableMountCache() override
    {
        return TableMountCache_;
    }

    const IReplicationCardCachePtr& GetReplicationCardCache() override
    {
        if (!ReplicationCardCache_) {
            THROW_ERROR_EXCEPTION("Replication card cache is not configured");
        }
        return ReplicationCardCache_;
    }

    const ITimestampProviderPtr& GetTimestampProvider() override
    {
        return TimestampProvider_;
    }

    const IClockManagerPtr& GetClockManager() override
    {
        return ClockManager_;
    }

    const TJobShellDescriptorCachePtr& GetJobShellDescriptorCache() override
    {
        return JobShellDescriptorCache_;
    }

    const TPermissionCachePtr& GetPermissionCache() override
    {
        return PermissionCache_;
    }

    const TStickyGroupSizeCachePtr& GetStickyGroupSizeCache() override
    {
        return StickyGroupSizeCache_;
    }

    const TSyncReplicaCachePtr& GetSyncReplicaCache() override
    {
        return SyncReplicaCache_;
    }

    const TTabletSyncReplicaCachePtr& GetTabletSyncReplicaCache() override
    {
        return TabletSyncReplicaCache_;
    }

    const NChaosClient::IBannedReplicaTrackerCachePtr& GetBannedReplicaTrackerCache() override
    {
        return BannedReplicaTrackerCache_;
    }

    IInvokerPtr GetInvoker() override
    {
        return Options_.ConnectionInvoker;
    }

    NApi::IClientPtr CreateClient(const TClientOptions& options) override
    {
        return NNative::CreateClient(this, options);
    }

    void ClearMetadataCaches() override
    {
        TableMountCache_->Clear();
        PermissionCache_->Clear();
    }

    // NNative::IConnection implementation.

    const TConnectionConfigPtr& GetConfig() const override
    {
        return Config_;
    }

    TConnectionDynamicConfigPtr GetDynamicConfig() const override
    {
        return DynamicConfig_.Load();
    }

    const TNetworkPreferenceList& GetNetworks() const override
    {
        return Config_->Networks ? *Config_->Networks : DefaultNetworkPreferences;
    }

    TCellId GetPrimaryMasterCellId() const override
    {
        return MasterCellDirectory_->GetPrimaryMasterCellId();
    }

    TCellTag GetPrimaryMasterCellTag() const override
    {
        return MasterCellDirectory_->GetPrimaryMasterCellTag();
    }

    const TCellTagList& GetSecondaryMasterCellTags() const override
    {
        return MasterCellDirectory_->GetSecondaryMasterCellTags();
    }

    TCellId GetMasterCellId(TCellTag cellTag) const override
    {
        return ReplaceCellTagInId(GetPrimaryMasterCellId(), cellTag);
    }

    IChannelPtr GetMasterChannelOrThrow(
        EMasterChannelKind kind,
        TCellTag cellTag = PrimaryMasterCellTagSentinel) override
    {
        return MasterCellDirectory_->GetMasterChannelOrThrow(kind, cellTag);
    }

    IChannelPtr GetMasterChannelOrThrow(
        EMasterChannelKind kind,
        TCellId cellId) override
    {
        return MasterCellDirectory_->GetMasterChannelOrThrow(kind, cellId);
    }

    const IChannelPtr& GetSchedulerChannel() override
    {
        return SchedulerChannel_;
    }

    const IChannelPtr& GetQueueAgentChannelOrThrow(TStringBuf stage) const override
    {
        auto it = QueueAgentChannels_.find(stage);
        if (it == QueueAgentChannels_.end()) {
            THROW_ERROR_EXCEPTION("Queue agent stage %Qv is not found", stage);
        }
        return it->second;
    }

    const TQueueConsumerRegistrationManagerPtr& GetQueueConsumerRegistrationManager() const override
    {
        return QueueConsumerRegistrationManager_;
    }

    const IChannelPtr& GetYqlAgentChannelOrThrow() const override
    {
        if (!YqlAgentChannel_) {
            THROW_ERROR_EXCEPTION("YQL agent channel is not configured");
        }
        return YqlAgentChannel_;
    }

    const IChannelFactoryPtr& GetChannelFactory() override
    {
        return ChannelFactory_;
    }

    const IBlockCachePtr& GetBlockCache() override
    {
        return BlockCache_;
    }

    const IClientChunkMetaCachePtr& GetChunkMetaCache() override
    {
        return ChunkMetaCache_;
    }

    const IEvaluatorPtr& GetQueryEvaluator() override
    {
        return QueryEvaluator_;
    }

    const IColumnEvaluatorCachePtr& GetColumnEvaluatorCache() override
    {
        return ColumnEvaluatorCache_;
    }

    const NCellMasterClient::TCellDirectoryPtr& GetMasterCellDirectory() override
    {
        return MasterCellDirectory_;
    }

    const NCellMasterClient::TCellDirectorySynchronizerPtr& GetMasterCellDirectorySynchronizer() override
    {
        return MasterCellDirectorySynchronizer_;
    }

    const NHiveClient::ICellDirectoryPtr& GetCellDirectory() override
    {
        return CellDirectory_;
    }

    const NHiveClient::ICellDirectorySynchronizerPtr& GetCellDirectorySynchronizer() override
    {
        return CellDirectorySynchronizer_;
    }

    const NChaosClient::IChaosCellDirectorySynchronizerPtr& GetChaosCellDirectorySynchronizer() override
    {
        return ChaosCellDirectorySynchronizer_;
    }

    const IReplicationCardChannelFactoryPtr& GetReplicationCardChannelFactory() override
    {
        return ReplicationCardChannelFactory_;
    }

    const TNodeDirectoryPtr& GetNodeDirectory() override
    {
        return NodeDirectory_;
    }

    const INodeDirectorySynchronizerPtr& GetNodeDirectorySynchronizer() override
    {
        return NodeDirectorySynchronizer_;
    }

    const NChunkClient::IChunkReplicaCachePtr& GetChunkReplicaCache() override
    {
        return ChunkReplicaCache_;
    }

    const TCellTrackerPtr& GetDownedCellTracker() override
    {
        return DownedCellTracker_;
    }

    const NHiveClient::TClusterDirectoryPtr& GetClusterDirectory() override
    {
        return ClusterDirectory_;
    }

    const NHiveClient::TClusterDirectorySynchronizerPtr& GetClusterDirectorySynchronizer() override
    {
        return ClusterDirectorySynchronizer_;
    }

    const NChunkClient::TMediumDirectoryPtr& GetMediumDirectory() override
    {
        return MediumDirectory_;
    }

    const NChunkClient::TMediumDirectorySynchronizerPtr& GetMediumDirectorySynchronizer() override
    {
        return MediumDirectorySynchronizer_;
    }

    IClientPtr CreateNativeClient(const TClientOptions& options) override
    {
        return NNative::CreateClient(this, options);
    }

    NHiveClient::ITransactionParticipantPtr CreateTransactionParticipant(
        TCellId cellId,
        const TTransactionParticipantOptions& options) override
    {
        // For tablet writes, manual sync is not needed since Table Mount Cache
        // is responsible for populating cell directory. Transaction participants,
        // on the other hand, have no other way to keep cell directory up-to-date.
        CellDirectorySynchronizer_->Start();
        return NNative::CreateTransactionParticipant(
            CellDirectory_,
            CellDirectorySynchronizer_,
            TimestampProvider_,
            this,
            cellId,
            options);
    }

    NDiscoveryClient::IDiscoveryClientPtr CreateDiscoveryClient(
        TDiscoveryClientConfigPtr clientConfig,
        IChannelFactoryPtr channelFactory) override
    {
        if (!Config_->DiscoveryConnection) {
            THROW_ERROR_EXCEPTION("Missing \"discovery_connection\" parameter in connection configuration");
        }

        return NDiscoveryClient::CreateDiscoveryClient(
            Config_->DiscoveryConnection,
            std::move(clientConfig),
            std::move(channelFactory));
    }

    virtual NDiscoveryClient::IMemberClientPtr CreateMemberClient(
        TMemberClientConfigPtr clientConfig,
        IChannelFactoryPtr channelFactory,
        IInvokerPtr invoker,
        TString id,
        TString groupId) override
    {
        if (!Config_->DiscoveryConnection) {
            THROW_ERROR_EXCEPTION("Missing \"discovery_connection\" parameter in connection configuration");
        }

        return NDiscoveryClient::CreateMemberClient(
            Config_->DiscoveryConnection,
            std::move(clientConfig),
            std::move(channelFactory),
            std::move(invoker),
            std::move(id),
            std::move(groupId));
    }

    IYPathServicePtr GetOrchidService() override
    {
        auto producer = BIND(&TConnection::BuildOrchid, MakeStrong(this));
        return IYPathService::FromProducer(producer);
    }

    void Terminate() override
    {
        Terminated_ = true;

        QueueConsumerRegistrationManager_->Clear();
        QueueConsumerRegistrationManager_->StopSync();

        ClusterDirectory_->Clear();
        ClusterDirectorySynchronizer_->Stop();

        CellDirectory_->Clear();
        CellDirectorySynchronizer_->Stop();
        ChaosCellDirectorySynchronizer_->Stop();

        MediumDirectory_->Clear();
        MediumDirectorySynchronizer_->Stop();

        NodeDirectorySynchronizer_->Stop();

        if (ReplicationCardCache_) {
            ReplicationCardCache_->Clear();
        }
    }

    bool IsTerminated() override
    {
        return Terminated_;
    }

    TFuture<void> SyncHiveCellWithOthers(
        const std::vector<TCellId>& srcCellIds,
        TCellId dstCellId) override
    {
        YT_LOG_DEBUG("Started synchronizing Hive cell with others (SrcCellIds: %v, DstCellId: %v)",
            srcCellIds,
            dstCellId);

        auto channel = CellDirectory_->GetChannelByCellIdOrThrow(dstCellId);
        THiveServiceProxy proxy(std::move(channel));

        auto req = proxy.SyncWithOthers();
        req->SetTimeout(Config_->HiveSyncRpcTimeout);
        ToProto(req->mutable_src_cell_ids(), srcCellIds);

        return req->Invoke()
            .Apply(BIND([=, this, this_ = MakeStrong(this)] (const THiveServiceProxy::TErrorOrRspSyncWithOthersPtr& rspOrError) {
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    rspOrError,
                    "Error synchronizing Hive cell %v with %v",
                    dstCellId,
                    srcCellIds);
                YT_LOG_DEBUG("Finished synchronizing Hive cell with others (SrcCellIds: %v, DstCellId: %v)",
                    srcCellIds,
                    dstCellId);
            }));
    }

    const NLogging::TLogger& GetLogger() override
    {
        return Logger;
    }

    void Reconfigure(const TConnectionDynamicConfigPtr& dynamicConfig) override
    {
        SyncReplicaCache_->Reconfigure(Config_->SyncReplicaCache->ApplyDynamic(dynamicConfig->SyncReplicaCache));
        TableMountCache_->Reconfigure(Config_->TableMountCache->ApplyDynamic(dynamicConfig->TableMountCache));
        ClockManager_->Reconfigure(Config_->ClockManager->ApplyDynamic(dynamicConfig->ClockManager));

        DynamicConfig_.Store(dynamicConfig);
    }

private:
    const TConnectionConfigPtr Config_;
    TAtomicObject<TConnectionDynamicConfigPtr> DynamicConfig_ = New<TConnectionDynamicConfig>();

    TConnectionOptions Options_;

    const TString LoggingTag_;
    const TString ClusterId_;

    NRpc::IChannelFactoryPtr ChannelFactory_;
    const TStickyGroupSizeCachePtr StickyGroupSizeCache_;

    const NLogging::TLogger Logger;
    const TProfiler Profiler_;

    const TTabletSyncReplicaCachePtr TabletSyncReplicaCache_;
    const IBannedReplicaTrackerCachePtr BannedReplicaTrackerCache_;

    // NB: There're also CellDirectory_ and CellDirectorySynchronizer_, which are completely different from these.
    NCellMasterClient::TCellDirectoryPtr MasterCellDirectory_;
    NCellMasterClient::TCellDirectorySynchronizerPtr MasterCellDirectorySynchronizer_;

    IChannelPtr SchedulerChannel_;
    THashMap<TString, IChannelPtr> QueueAgentChannels_;
    TQueueConsumerRegistrationManagerPtr QueueConsumerRegistrationManager_;
    IChannelPtr YqlAgentChannel_;
    IBlockCachePtr BlockCache_;
    IClientChunkMetaCachePtr ChunkMetaCache_;
    ITableMountCachePtr TableMountCache_;
    IReplicationCardCachePtr ReplicationCardCache_;
    IChannelPtr TimestampProviderChannel_;
    ITimestampProviderPtr TimestampProvider_;
    IClockManagerPtr ClockManager_;
    TJobShellDescriptorCachePtr JobShellDescriptorCache_;
    TPermissionCachePtr PermissionCache_;
    IEvaluatorPtr QueryEvaluator_;
    IColumnEvaluatorCachePtr ColumnEvaluatorCache_;
    TSyncReplicaCachePtr SyncReplicaCache_;

    ICellDirectoryPtr CellDirectory_;
    ICellDirectorySynchronizerPtr CellDirectorySynchronizer_;
    IChaosCellDirectorySynchronizerPtr ChaosCellDirectorySynchronizer_;
    const TCellTrackerPtr DownedCellTracker_ = New<TCellTracker>();

    TClusterDirectoryPtr ClusterDirectory_;
    TClusterDirectorySynchronizerPtr ClusterDirectorySynchronizer_;

    TMediumDirectoryPtr MediumDirectory_;
    TMediumDirectorySynchronizerPtr MediumDirectorySynchronizer_;

    TNodeDirectoryPtr NodeDirectory_;
    INodeDirectorySynchronizerPtr NodeDirectorySynchronizer_;

    IChunkReplicaCachePtr ChunkReplicaCache_;

    IThreadPoolPtr ConnectionThreadPool_;

    IReplicationCardChannelFactoryPtr ReplicationCardChannelFactory_;

    std::atomic<bool> Terminated_ = false;

    void ConfigureMasterCells()
    {
        CellDirectory_->ReconfigureCell(Config_->PrimaryMaster);
        for (const auto& cellConfig : Config_->SecondaryMasters) {
            CellDirectory_->ReconfigureCell(cellConfig);
        }
    }

    TCellIdList GetCellDirectorySynchronizerSourceOfTruthCellIds()
    {
        // For single-cell clusters we have to sync with the primary cell.
        // For multicell clusters we sync with a random secondary cell each time
        // to reduce load on the primary cell.
        TCellIdList cellIds;
        if (Config_->CellDirectorySynchronizer->SyncCellsWithSecondaryMasters) {
            cellIds = MasterCellDirectory_->GetSecondaryMasterCellIds();
        }
        if (cellIds.empty()) {
            cellIds.push_back(GetPrimaryMasterCellId());
        }
        return cellIds;
    }

    void BuildOrchid(IYsonConsumer* consumer)
    {
        bool hasMasterCache = static_cast<bool>(Config_->MasterCache);
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("master_cache")
                    .BeginMap()
                        .Item("enabled").Value(hasMasterCache)
                        .DoIf(hasMasterCache, [this] (auto fluent) {
                            auto masterCacheChannel = MasterCellDirectory_->GetMasterChannelOrThrow(
                                EMasterChannelKind::Cache,
                                MasterCellDirectory_->GetPrimaryMasterCellId());
                            fluent
                                .Item("channel_attributes").Value(masterCacheChannel->GetEndpointAttributes());
                        })
                    .EndMap()
                .Item("timestamp_provider")
                    .BeginMap()
                        .Item("channel_attributes").Value(TimestampProviderChannel_->GetEndpointAttributes())
                    .EndMap()
            .EndMap();
    }

    void InitializeTimestampProvider()
    {
        auto timestampProviderConfig = Config_->TimestampProvider;
        if (!timestampProviderConfig) {
            timestampProviderConfig = CreateRemoteTimestampProviderConfig(Config_->PrimaryMaster);
        }

        TimestampProviderChannel_ = timestampProviderConfig->EnableTimestampProviderDiscovery ?
            CreateNodeAddressesChannel(
                timestampProviderConfig->TimestampProviderDiscoveryPeriod,
                timestampProviderConfig->TimestampProviderDiscoveryPeriodSplay,
                MakeWeak(MasterCellDirectory_),
                ENodeRole::TimestampProvider,
                BIND(&CreateTimestampProviderChannelFromAddresses, timestampProviderConfig, ChannelFactory_)) :
            CreateTimestampProviderChannel(timestampProviderConfig, ChannelFactory_);
        TimestampProvider_ = CreateBatchingRemoteTimestampProvider(timestampProviderConfig, TimestampProviderChannel_);
    }

    void InitializeQueueAgentChannels()
    {
        for (const auto& [stage, channelConfig] : Config_->QueueAgent->Stages) {
            auto endpointDescription = Format("QueueAgent/%v", stage);
            auto endpointAttributes = ConvertToAttributes(BuildYsonStringFluently()
                .BeginMap()
                    .Item("queue_agent").Value(true)
                    .Item("stage").Value(stage)
                .EndMap());

            auto channel = CreateBalancingChannel(
                channelConfig,
                ChannelFactory_,
                std::move(endpointDescription),
                std::move(endpointAttributes));

            channel = CreateRetryingChannel(channelConfig, std::move(channel));

            // TODO(max42): make customizable.
            constexpr auto timeout = TDuration::Minutes(1);
            channel = CreateDefaultTimeoutChannel(std::move(channel), timeout);

            QueueAgentChannels_[stage] = std::move(channel);
        }
    }

    void InitializeYqlAgentChannel()
    {
        if (!Config_->YqlAgent) {
            return;
        }

        auto endpointDescription = "YqlAgent";
        auto endpointAttributes = ConvertToAttributes(BuildYsonStringFluently()
            .BeginMap()
                .Item("yql_agent").Value(true)
            .EndMap());

        auto channel = CreateBalancingChannel(
            Config_->YqlAgent->Channel,
            ChannelFactory_,
            std::move(endpointDescription),
            std::move(endpointAttributes));

        // TODO(max42): make customizable.
        constexpr auto timeout = TDuration::Days(1);
        channel = CreateDefaultTimeoutChannel(std::move(channel), timeout);

        YqlAgentChannel_ = std::move(channel);
    }

    void SetupTvmIdSynchronization()
    {
        auto tvmService = Options_.TvmService;
        if (!tvmService) {
            tvmService = TNativeAuthenticationManager::Get()->GetTvmService();
        }
        if (!tvmService) {
            return;
        }
        if (Config_->TvmId) {
            tvmService->AddDestinationServiceIds({*Config_->TvmId});
        }
        ClusterDirectory_->SubscribeOnClusterUpdated(
            BIND_NO_PROPAGATE([tvmService] (const TString& name, INodePtr nativeConnectionConfig) {
                static const auto& Logger = TvmSynchronizerLogger;

                NNative::TConnectionConfigPtr config;
                try {
                    config = ConvertTo<NNative::TConnectionConfigPtr>(nativeConnectionConfig);
                } catch (const std::exception& ex) {
                    YT_LOG_ERROR(ex, "Cannot update cluster TVM ids because of invalid connection config (Name: %v)", name);
                    return;
                }

                if (config->TvmId) {
                    YT_LOG_INFO("Adding cluster service ticket to TVM client (Name: %v, TvmId: %v)", name, *config->TvmId);
                    tvmService->AddDestinationServiceIds({*config->TvmId});
                }
            }));
    }
};

TConnectionOptions::TConnectionOptions(IInvokerPtr invoker)
{
    ConnectionInvoker = std::move(invoker);
}

IConnectionPtr CreateConnection(
    TConnectionConfigPtr config,
    TConnectionOptions options)
{
    NTracing::TNullTraceContextGuard nullTraceContext;

    if (!config->PrimaryMaster) {
        THROW_ERROR_EXCEPTION("Missing \"primary_master\" parameter in connection configuration");
    }
    auto connection = New<TConnection>(std::move(config), std::move(options));
    connection->Initialize();
    return connection;
}

////////////////////////////////////////////////////////////////////////////////

TStickyGroupSizeCache::TKey::operator size_t() const
{
    size_t result = 0;
    HashCombine(result, Key);
    for (const auto& part : Message) {
        HashCombine(result, GetChecksum(part));
    }
    return result;
}

bool TStickyGroupSizeCache::TKey::operator == (const TKey& other) const
{
    if (Key != other.Key || Message.Size() != other.Message.Size()) {
        return false;
    }
    for (int i = 0; i < static_cast<int>(Message.Size()); ++i) {
        if (!TRef::AreBitwiseEqual(Message[i], other.Message[i])) {
            return false;
        }
    }
    return true;
}

TStickyGroupSizeCache::TStickyGroupSizeCache(TDuration expirationTimeout)
    : AdvisedStickyGroupSize_(New<TSyncExpiringCache<TKey, std::optional<int>>>(
        BIND([] (const TKey& /*key*/) {
            return std::optional<int>{};
        }),
        expirationTimeout,
        GetSyncInvoker()))
{ }

void TStickyGroupSizeCache::UpdateAdvisedStickyGroupSize(const TKey& key, int stickyGroupSize)
{
    AdvisedStickyGroupSize_->Set(key, stickyGroupSize);
}

std::optional<int> TStickyGroupSizeCache::GetAdvisedStickyGroupSize(const TKey& key)
{
    auto result = AdvisedStickyGroupSize_->Find(key);
    return result.value_or(std::nullopt);
}

////////////////////////////////////////////////////////////////////////////////

IConnectionPtr FindRemoteConnection(
    const IConnectionPtr& connection,
    const TString& clusterName)
{
    return connection->GetClusterDirectory()->FindConnection(clusterName);
}

IConnectionPtr FindRemoteConnection(
    const IConnectionPtr& connection,
    const std::optional<TString>& clusterName)
{
    if (clusterName) {
        if (auto remoteConnection = connection->GetClusterDirectory()->FindConnection(*clusterName)) {
            return remoteConnection;
        }
    }
    return connection;
}

IConnectionPtr GetRemoteConnectionOrThrow(
    const IConnectionPtr& connection,
    const TString& clusterName,
    bool syncOnFailure)
{
    for (int retry = 0; retry < 2; ++retry) {
        auto remoteConnection = FindRemoteConnection(connection, clusterName);
        if (remoteConnection) {
            return remoteConnection;
        }

        if (!syncOnFailure || retry == 1) {
            THROW_ERROR_EXCEPTION("Cannot find cluster with name %Qv", clusterName);
        }

        WaitFor(connection->GetClusterDirectorySynchronizer()->Sync(/*force*/ true))
            .ThrowOnError();
    }

    YT_ABORT();
}

IConnectionPtr FindRemoteConnection(
    const IConnectionPtr& connection,
    TCellTag cellTag)
{
    if (cellTag == connection->GetPrimaryMasterCellTag()) {
        return connection;
    }

    const auto& secondaryCellTags = connection->GetSecondaryMasterCellTags();
    if (std::find(secondaryCellTags.begin(), secondaryCellTags.end(), cellTag) != secondaryCellTags.end()) {
        return connection;
    }

    return connection->GetClusterDirectory()->FindConnection(cellTag);
}

IConnectionPtr GetRemoteConnectionOrThrow(
    const IConnectionPtr& connection,
    TCellTag cellTag)
{
    auto remoteConnection = FindRemoteConnection(connection, cellTag);
    if (!remoteConnection) {
        THROW_ERROR_EXCEPTION("Cannot find cluster with cell tag %v", cellTag);
    }
    return remoteConnection;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
