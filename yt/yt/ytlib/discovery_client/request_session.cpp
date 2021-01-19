#include "request_session.h"
#include "discovery_client_service_proxy.h"
#include "helpers.h"

#include <yt/core/misc/public.h>

#include <yt/core/rpc/public.h>
#include <yt/core/rpc/retrying_channel.h>

#include <yt/core/concurrency/delayed_executor.h>

namespace NYT::NDiscoveryClient {

using namespace NConcurrency;
using namespace NRpc;
using namespace NYTree;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TDiscoveryClientServiceProxy CreateProxy(
    const TDiscoveryClientConfigPtr& config,
    const IChannelFactoryPtr& channelFactory,
    const TString& address)
{
    auto channel = channelFactory->CreateChannel(address);
    TDiscoveryClientServiceProxy proxy(CreateRetryingChannel(config, std::move(channel)));
    proxy.SetDefaultTimeout(config->RpcTimeout);
    return proxy;
}

////////////////////////////////////////////////////////////////////////////////

TListMembersRequestSession::TListMembersRequestSession(
    TServerAddressPoolPtr addressPool,
    TDiscoveryClientConfigPtr config,
    IChannelFactoryPtr channelFactory,
    const NLogging::TLogger& logger,
    TGroupId groupId,
    TListMembersOptions options)
    : TRequestSession<std::vector<TMemberInfo>>(
        config->ReadQuorum,
        std::move(addressPool),
        logger)
    , Config_(std::move(config))
    , ChannelFactory_(std::move(channelFactory))
    , GroupId_(std::move(groupId))
    , Options_(std::move(options))
{ }

TFuture<void> TListMembersRequestSession::MakeRequest(const TString& address)
{
    auto proxy = CreateProxy(Config_, ChannelFactory_, address);

    auto req = proxy.ListMembers();
    req->set_group_id(GroupId_);
    ToProto(req->mutable_options(), Options_);

    return req->Invoke().Apply(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TDiscoveryClientServiceProxy::TRspListMembersPtr>& rspOrError){
        if (!rspOrError.IsOK() && !rspOrError.FindMatching(NDiscoveryClient::EErrorCode::NoSuchGroup)) {
            return TError(rspOrError);
        }
        auto guard = Guard(Lock_);
        if (rspOrError.IsOK()) {
            const auto& rsp = rspOrError.Value();
            for (const auto& protoMemberInfo : rsp->members()) {
                auto member = FromProto<TMemberInfo>(protoMemberInfo);
                if (auto it = IdToMember_.find(member.Id); it == IdToMember_.end()) {
                    YT_VERIFY(IdToMember_.emplace(member.Id, std::move(member)).second);
                } else if (it->second.Revision < member.Revision) {
                    it->second = std::move(member);
                }
            }
        }
        if (++SuccessCount_ == RequiredSuccessCount_) {
            std::vector<TMemberInfo> members;
            for (auto& [id, member] : IdToMember_) {
                members.emplace_back(std::move(member));
            }
            guard.Release();
            std::sort(members.begin(), members.end(), [] (const TMemberInfo& lhs, const TMemberInfo& rhs) {
                if (lhs.Priority != rhs.Priority) {
                    return lhs.Priority < rhs.Priority;
                }
                return lhs.Id < rhs.Id;
            });

            if (members.empty()) {
                Promise_.TrySet(TError(NDiscoveryClient::EErrorCode::NoSuchGroup, "Group %Qv does not exist", GroupId_));
            } else {
                Promise_.TrySet(std::move(members));
            }
        }
        return TError();
    }));
}

////////////////////////////////////////////////////////////////////////////////

TGetGroupMetaRequestSession::TGetGroupMetaRequestSession(
    TServerAddressPoolPtr addressPool,
    TDiscoveryClientConfigPtr config,
    IChannelFactoryPtr channelFactory,
    const NLogging::TLogger& logger,
    TGroupId groupId)
    : TRequestSession<TGroupMeta>(
        config->ReadQuorum,
        std::move(addressPool),
        logger)
    , Config_(std::move(config))
    , ChannelFactory_(std::move(channelFactory))
    , GroupId_(std::move(groupId))
{ }

TFuture<void> TGetGroupMetaRequestSession::MakeRequest(const TString& address)
{
    auto proxy = CreateProxy(Config_, ChannelFactory_, address);

    auto req = proxy.GetGroupMeta();
    req->set_group_id(GroupId_);
    return req->Invoke().Apply(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TDiscoveryClientServiceProxy::TRspGetGroupMetaPtr>& rspOrError) {
        if (!rspOrError.IsOK() && !rspOrError.FindMatching(NDiscoveryClient::EErrorCode::NoSuchGroup)) {
            return TError(rspOrError);
        }

        auto guard = Guard(Lock_);

        if (rspOrError.IsOK()) {
            auto groupMeta = FromProto<TGroupMeta>(rspOrError.Value()->meta());
            GroupMeta_.MemberCount = std::max(GroupMeta_.MemberCount, groupMeta.MemberCount);
        }

        if (++SuccessCount_ == RequiredSuccessCount_) {
            auto groupMeta = GroupMeta_;
            guard.Release();
            if (groupMeta.MemberCount == 0) {
                Promise_.TrySet(TError(NDiscoveryClient::EErrorCode::NoSuchGroup, "Group %Qv does not exist", GroupId_));
            } else {
                Promise_.TrySet(GroupMeta_);
            }
        }
        return TError();
    }));
}

////////////////////////////////////////////////////////////////////////////////

THeartbeatSession::THeartbeatSession(
    TServerAddressPoolPtr addressPool,
    TMemberClientConfigPtr config,
    IChannelFactoryPtr channelFactory,
    const NLogging::TLogger& logger,
    TGroupId groupId,
    TMemberId memberId,
    i64 priority,
    i64 revision,
    IAttributeDictionaryPtr attributes)
    : TRequestSession<void>(
        config->WriteQuorum,
        std::move(addressPool),
        logger)
    , Config_(std::move(config))
    , ChannelFactory_(std::move(channelFactory))
    , GroupId_(std::move(groupId))
    , MemberId_(std::move(memberId))
    , Priority_(priority)
    , Revision_(revision)
    , Attributes_(std::move(attributes))
{ }

TFuture<void> THeartbeatSession::MakeRequest(const TString& address)
{
    auto channel = ChannelFactory_->CreateChannel(address);
    TDiscoveryClientServiceProxy proxy(std::move(channel));
    proxy.SetDefaultTimeout(Config_->RpcTimeout);

    auto req = proxy.Heartbeat();

    req->set_group_id(GroupId_);
    auto* protoMemberInfo = req->mutable_member_info();
    protoMemberInfo->set_id(MemberId_);
    protoMemberInfo->set_priority(Priority_);
    protoMemberInfo->set_revision(Revision_);
    if (Attributes_) {
        ToProto(protoMemberInfo->mutable_attributes(), *Attributes_);
    }
    req->set_lease_timeout(ToProto<i64>(Config_->LeaseTimeout));

    return req->Invoke().Apply(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TDiscoveryClientServiceProxy::TRspHeartbeatPtr>& rspOrError) {
        if (!rspOrError.IsOK()) {
            if (rspOrError.FindMatching(NDiscoveryClient::EErrorCode::InvalidGroupId) ||
                rspOrError.FindMatching(NDiscoveryClient::EErrorCode::InvalidMemberId))
            {
                Promise_.TrySet(rspOrError);
                return TError();
            } else {
                return TError(rspOrError);
            }
        }

        if (++SuccessCount_ == RequiredSuccessCount_) {
            Promise_.TrySet();
        }
        return TError();
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryClient
