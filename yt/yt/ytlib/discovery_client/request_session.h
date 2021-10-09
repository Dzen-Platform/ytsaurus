#pragma once

#include "public.h"
#include "helpers.h"
#include "config.h"

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/rpc/request_session.h>

namespace NYT::NDiscoveryClient {

////////////////////////////////////////////////////////////////////////////////

TDiscoveryClientServiceProxy CreateProxy(
    const TDiscoveryClientConfigPtr& config,
    const NRpc::IChannelFactoryPtr& channelFactory,
    const TString& address);

////////////////////////////////////////////////////////////////////////////////

class TListMembersRequestSession
    : public NRpc::TRequestSession<std::vector<TMemberInfo>>
{
public:
    TListMembersRequestSession(
        NRpc::TServerAddressPoolPtr addressPool,
        TDiscoveryClientConfigPtr config,
        NRpc::IChannelFactoryPtr channelFactory,
        const NLogging::TLogger& logger,
        TGroupId groupId,
        TListMembersOptions options);

private:
    const TDiscoveryClientConfigPtr Config_;
    const NRpc::IChannelFactoryPtr ChannelFactory_;
    const TGroupId GroupId_;
    const TListMembersOptions Options_;

    YT_DECLARE_SPINLOCK(TAdaptiveLock, Lock_);
    THashMap<TMemberId, TMemberInfo> IdToMember_;
    int SuccessCount_ = 0;

    TFuture<void> MakeRequest(const TString& address) override;
};

DEFINE_REFCOUNTED_TYPE(TListMembersRequestSession)

////////////////////////////////////////////////////////////////////////////////

class TGetGroupMetaRequestSession
    : public NRpc::TRequestSession<TGroupMeta>
{
public:
    TGetGroupMetaRequestSession(
        NRpc::TServerAddressPoolPtr addressPool,
        TDiscoveryClientConfigPtr config,
        NRpc::IChannelFactoryPtr channelFactory,
        const NLogging::TLogger& logger,
        TGroupId groupId);

private:
    const TDiscoveryClientConfigPtr Config_;
    const NRpc::IChannelFactoryPtr ChannelFactory_;
    const TGroupId GroupId_;

    YT_DECLARE_SPINLOCK(TAdaptiveLock, Lock_);
    TGroupMeta GroupMeta_;
    int SuccessCount_ = 0;

    TFuture<void> MakeRequest(const TString& address) override;
};

DEFINE_REFCOUNTED_TYPE(TGetGroupMetaRequestSession)

////////////////////////////////////////////////////////////////////////////////

class THeartbeatSession
    : public NRpc::TRequestSession<void>
{
public:
    THeartbeatSession(
        NRpc::TServerAddressPoolPtr addressPool,
        TMemberClientConfigPtr config,
        NRpc::IChannelFactoryPtr channelFactory,
        const NLogging::TLogger& logger,
        TGroupId groupId,
        TMemberId memberId,
        i64 priority,
        i64 revision,
        NYTree::IAttributeDictionaryPtr attributes);

private:
    const TMemberClientConfigPtr Config_;
    const NRpc::IChannelFactoryPtr ChannelFactory_;
    const TGroupId GroupId_;
    const TMemberId MemberId_;
    const i64 Priority_;
    const i64 Revision_;
    const NYTree::IAttributeDictionaryPtr Attributes_;

    std::atomic<int> SuccessCount_ = 0;

    TFuture<void> MakeRequest(const TString& address) override;
};

DEFINE_REFCOUNTED_TYPE(THeartbeatSession)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryClient

