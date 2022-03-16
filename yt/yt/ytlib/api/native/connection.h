#pragma once

#include "public.h"

#include <yt/yt/client/api/connection.h>

#include <yt/yt/ytlib/cell_master_client/public.h>

#include <yt/yt/ytlib/chaos_client/public.h>

#include <yt/yt/ytlib/query_client/public.h>

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/ytlib/job_prober_client/public.h>

#include <yt/yt/ytlib/security_client/public.h>

#include <yt/yt/ytlib/hive/public.h>

#include <yt/yt/client/chaos_client/public.h>

#include <yt/yt/core/logging/public.h>

#include <yt/yt/core/misc/ref.h>
#include <yt/yt/core/misc/sync_expiring_cache.h>

namespace NYT::NApi::NNative {

////////////////////////////////////////////////////////////////////////////////

struct IConnection
    : public NApi::IConnection
{
    virtual const TConnectionConfigPtr& GetConfig() = 0;

    virtual const NNodeTrackerClient::TNetworkPreferenceList& GetNetworks() const = 0;

    virtual NObjectClient::TCellId GetPrimaryMasterCellId() const = 0;
    virtual NObjectClient::TCellTag GetPrimaryMasterCellTag() const = 0;
    virtual const NObjectClient::TCellTagList& GetSecondaryMasterCellTags() const = 0;
    virtual NObjectClient::TCellId GetMasterCellId(NObjectClient::TCellTag cellTag) const = 0;

    virtual const NQueryClient::IEvaluatorPtr& GetQueryEvaluator() = 0;
    virtual const NQueryClient::IColumnEvaluatorCachePtr& GetColumnEvaluatorCache() = 0;
    virtual const NChunkClient::IBlockCachePtr& GetBlockCache() = 0;
    virtual const NChunkClient::IClientChunkMetaCachePtr& GetChunkMetaCache() = 0;

    virtual const NCellMasterClient::TCellDirectoryPtr& GetMasterCellDirectory() = 0;
    virtual const NCellMasterClient::TCellDirectorySynchronizerPtr& GetMasterCellDirectorySynchronizer() = 0;

    virtual const NHiveClient::ICellDirectoryPtr& GetCellDirectory() = 0;
    virtual const NHiveClient::ICellDirectorySynchronizerPtr& GetCellDirectorySynchronizer() = 0;
    virtual const NChaosClient::IChaosCellDirectorySynchronizerPtr& GetChaosCellDirectorySynchronizer() = 0;

    virtual const NHiveClient::TClusterDirectoryPtr& GetClusterDirectory() = 0;
    virtual const NHiveClient::TClusterDirectorySynchronizerPtr& GetClusterDirectorySynchronizer() = 0;

    virtual const NChunkClient::TMediumDirectoryPtr& GetMediumDirectory() = 0;
    virtual const NChunkClient::TMediumDirectorySynchronizerPtr& GetMediumDirectorySynchronizer() = 0;

    virtual const NNodeTrackerClient::TNodeDirectoryPtr& GetNodeDirectory() = 0;
    virtual const NNodeTrackerClient::TNodeDirectorySynchronizerPtr& GetNodeDirectorySynchronizer() = 0;

    virtual const NChunkClient::IChunkReplicaCachePtr& GetChunkReplicaCache() = 0;

    virtual const NHiveClient::TCellTrackerPtr& GetDownedCellTracker() = 0;

    virtual NRpc::IChannelPtr GetMasterChannelOrThrow(
        EMasterChannelKind kind,
        NObjectClient::TCellTag cellTag = NObjectClient::PrimaryMasterCellTagSentinel) = 0;
    virtual NRpc::IChannelPtr GetMasterChannelOrThrow(
        EMasterChannelKind kind,
        NObjectClient::TCellId cellId) = 0;
    virtual const NRpc::IChannelPtr& GetSchedulerChannel() = 0;
    virtual const NRpc::IChannelFactoryPtr& GetChannelFactory() = 0;

    virtual const NRpc::IChannelPtr& GetQueueAgentChannelOrThrow(TStringBuf stage) const = 0;

    virtual const NTabletClient::ITableMountCachePtr& GetTableMountCache() = 0;
    virtual const NChaosClient::IReplicationCardCachePtr& GetReplicationCardCache() = 0;
    virtual const NTransactionClient::ITimestampProviderPtr& GetTimestampProvider() = 0;

    virtual const NJobProberClient::TJobShellDescriptorCachePtr& GetJobShellDescriptorCache() = 0;

    virtual const NSecurityClient::TPermissionCachePtr& GetPermissionCache() = 0;

    virtual const TStickyGroupSizeCachePtr& GetStickyGroupSizeCache() = 0;

    virtual const TSyncReplicaCachePtr& GetSyncReplicaCache() = 0;
    virtual const TTabletSyncReplicaCachePtr& GetTabletSyncReplicaCache() = 0;

    virtual IClientPtr CreateNativeClient(const TClientOptions& options) = 0;

    virtual NYTree::IYPathServicePtr GetOrchidService() = 0;

    void Terminate() override = 0;
    virtual bool IsTerminated() = 0;

    virtual TFuture<void> SyncHiveCellWithOthers(
        const std::vector<NElection::TCellId>& srcCellIds,
        NElection::TCellId dstCellId) = 0;

    virtual const NLogging::TLogger& GetLogger() = 0;

    virtual void Reconfigure(const TConnectionDynamicConfigPtr& dynamicConfig) = 0;
};

DEFINE_REFCOUNTED_TYPE(IConnection)

////////////////////////////////////////////////////////////////////////////////

class TStickyGroupSizeCache
    : public TRefCounted
{
public:
    struct TKey
    {
        std::optional<TString> Key;
        TSharedRefArray Message;

        operator size_t() const;
        bool operator == (const TKey& other) const;
    };

    TStickyGroupSizeCache(TDuration expirationTimeout = TDuration::Seconds(30));

    void UpdateAdvisedStickyGroupSize(const TKey& key, int stickyGroupSize);
    std::optional<int> GetAdvisedStickyGroupSize(const TKey& key);

private:
    const TIntrusivePtr<TSyncExpiringCache<TKey, std::optional<int>>> AdvisedStickyGroupSize_;
};

DEFINE_REFCOUNTED_TYPE(TStickyGroupSizeCache)

////////////////////////////////////////////////////////////////////////////////

struct TConnectionOptions
    : public NApi::TConnectionOptions
{
    bool RetryRequestQueueSizeLimitExceeded = false;

    //! If non-null, provides an externally-controlled block cache.
    NChunkClient::IBlockCachePtr BlockCache;

    //! If non-null, provides an externally-controlled chunk meta cache.
    NChunkClient::IClientChunkMetaCachePtr ChunkMetaCache;
};

//! Native connection talks directly to the cluster via internal
//! (and typically not stable) RPC protocols.
IConnectionPtr CreateConnection(
    TConnectionConfigPtr config,
    TConnectionOptions options = {});

////////////////////////////////////////////////////////////////////////////////

IConnectionPtr FindRemoteConnection(
    const IConnectionPtr& connection,
    const TString& clusterName);

IConnectionPtr GetRemoteConnectionOrThrow(
    const NApi::NNative::IConnectionPtr& connection,
    const TString& clusterName,
    bool syncOnFailure = false);

IConnectionPtr FindRemoteConnection(
    const NApi::NNative::IConnectionPtr& connection,
    NObjectClient::TCellTag cellTag);

IConnectionPtr GetRemoteConnectionOrThrow(
    const NApi::NNative::IConnectionPtr& connection,
    NObjectClient::TCellTag cellTag);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
