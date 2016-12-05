#pragma once

#include "public.h"

#include <yt/ytlib/scheduler/scheduler_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TSchedulerServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TSchedulerServiceProxy, RPC_PROXY_DESC(SchedulerService)
        .SetProtocolVersion(4));

    DEFINE_RPC_PROXY_METHOD(NScheduler::NProto, StartOperation);
    DEFINE_RPC_PROXY_METHOD(NScheduler::NProto, AbortOperation);
    DEFINE_RPC_PROXY_METHOD(NScheduler::NProto, SuspendOperation);
    DEFINE_RPC_PROXY_METHOD(NScheduler::NProto, ResumeOperation);
    DEFINE_RPC_PROXY_METHOD(NScheduler::NProto, CompleteOperation);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
