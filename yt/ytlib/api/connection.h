#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/hive/public.h>

#include <yt/ytlib/query_client/public.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/actions/callback.h>

#include <yt/core/rpc/public.h>

#include <yt/ytlib/object_client/public.h>

namespace NYT {
namespace NApi {

////////////////////////////////////////////////////////////////////////////////

struct TAdminOptions
{ };

struct TClientOptions
{
    explicit TClientOptions(const Stroka& user =  NSecurityClient::GuestUserName)
        : User(user)
    { }

    Stroka User;
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMasterChannelKind,
    (Leader)
    (Follower)
    (LeaderOrFollower)
    (Cache)
);

//! Represents an established connection with a YT cluster.
/*
 *  IConnection instance caches most of the stuff needed for fast interaction
 *  with the cluster (e.g. connection channels, mount info etc).
 *  
 *  Thread affinity: any
 */
struct IConnection
    : public virtual TRefCounted
{
    virtual TConnectionConfigPtr GetConfig() = 0;

    virtual const NObjectClient::TCellId& GetPrimaryMasterCellId() const = 0;
    virtual NObjectClient::TCellTag GetPrimaryMasterCellTag() const = 0;
    virtual const std::vector<NObjectClient::TCellTag>& GetSecondaryMasterCellTags() const = 0;

    virtual NRpc::IChannelPtr GetMasterChannel(
        EMasterChannelKind kind,
        NObjectClient::TCellTag cellTag = NObjectClient::PrimaryMasterCellTag) = 0;
    virtual NRpc::IChannelPtr GetSchedulerChannel() = 0;
    virtual NRpc::IChannelFactoryPtr GetNodeChannelFactory() = 0;

    virtual NChunkClient::IBlockCachePtr GetBlockCache() = 0;
    virtual NTabletClient::TTableMountCachePtr GetTableMountCache() = 0;
    virtual NTransactionClient::ITimestampProviderPtr GetTimestampProvider() = 0;
    virtual NHive::TCellDirectoryPtr GetCellDirectory() = 0;
    virtual NQueryClient::IFunctionRegistryPtr GetFunctionRegistry() = 0;
    virtual NQueryClient::TEvaluatorPtr GetQueryEvaluator() = 0;
    virtual NQueryClient::TColumnEvaluatorCachePtr GetColumnEvaluatorCache() = 0;

    virtual IAdminPtr CreateAdmin(const TAdminOptions& options = TAdminOptions()) = 0;

    virtual IClientPtr CreateClient(const TClientOptions& options = TClientOptions()) = 0;

    virtual void ClearMetadataCaches() = 0;

};

DEFINE_REFCOUNTED_TYPE(IConnection)

////////////////////////////////////////////////////////////////////////////////

struct TConnectionOptions
{
    bool RetryRequestRateLimitExceeded = false;
};

IConnectionPtr CreateConnection(
    TConnectionConfigPtr config,
    const TConnectionOptions& options = TConnectionOptions());

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

