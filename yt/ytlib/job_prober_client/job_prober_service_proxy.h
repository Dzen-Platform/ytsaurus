#pragma once

#include "public.h"

#include <yt/ytlib/job_prober_client/job_prober_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NJobProberClient {

////////////////////////////////////////////////////////////////////////////////

class TJobProberServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TJobProberServiceProxy, JobProberService,
        .SetProtocolVersion(0));

    DEFINE_RPC_PROXY_METHOD(NJobProberClient::NProto, DumpInputContext);
    DEFINE_RPC_PROXY_METHOD(NJobProberClient::NProto, GetStderr);
    DEFINE_RPC_PROXY_METHOD(NJobProberClient::NProto, Strace);
    DEFINE_RPC_PROXY_METHOD(NJobProberClient::NProto, SignalJob);
    DEFINE_RPC_PROXY_METHOD(NJobProberClient::NProto, PollJobShell);
    DEFINE_RPC_PROXY_METHOD(NJobProberClient::NProto, Interrupt);
    DEFINE_RPC_PROXY_METHOD(NJobProberClient::NProto, Fail);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProberClient
} // namespace NYT
