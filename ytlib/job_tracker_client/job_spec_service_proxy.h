#pragma once

#include "public.h"

#include <yt/ytlib/job_tracker_client/job_spec_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NJobTrackerClient {

////////////////////////////////////////////////////////////////////////////////

class TJobSpecServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TJobSpecServiceProxy, RPC_PROXY_DESC(JobSpecService));

    DEFINE_RPC_PROXY_METHOD(NProto, GetJobSpecs);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobTrackerClient
} // namespace NYT
