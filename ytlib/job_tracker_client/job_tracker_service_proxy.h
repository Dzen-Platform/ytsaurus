#pragma once

#include "public.h"

#include <yt/ytlib/job_tracker_client/job_tracker_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NJobTrackerClient {

////////////////////////////////////////////////////////////////////////////////

class TJobTrackerServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TJobTrackerServiceProxy, RPC_PROXY_DESC(JobTrackerService)
        .SetProtocolVersion(11));

    DEFINE_RPC_PROXY_METHOD(NProto, Heartbeat);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobTrackerClient
} // namespace NYT
