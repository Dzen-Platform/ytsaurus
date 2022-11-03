#pragma once

#include "public.h"

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/ytree/attributes.h>

#include <yt/yt/core/rpc/public.h>

namespace NYT::NDiscoveryClient {

////////////////////////////////////////////////////////////////////////////////

struct IMemberClient
    : public virtual TRefCounted
{
    virtual TFuture<void> Start() = 0;
    virtual TFuture<void> Stop() = 0;

    virtual void Reconfigure(TMemberClientConfigPtr config) = 0;

    virtual NYTree::IAttributeDictionary* GetAttributes() = 0;

    virtual i64 GetPriority() = 0;
    virtual void SetPriority(i64 value) = 0;
};

DEFINE_REFCOUNTED_TYPE(IMemberClient)

IMemberClientPtr CreateMemberClient(
    TDiscoveryConnectionConfigPtr connectionConfig,
    TMemberClientConfigPtr clientConfig,
    NRpc::IChannelFactoryPtr channelFactory,
    IInvokerPtr invoker,
    TString id,
    TString groupId);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryClient
