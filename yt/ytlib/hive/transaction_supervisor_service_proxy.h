#pragma once

#include "public.h"

#include <yt/ytlib/hive/transaction_supervisor_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NHiveClient {

////////////////////////////////////////////////////////////////////////////////

class TTransactionSupervisorServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TTransactionSupervisorServiceProxy, RPC_PROXY_DESC(JobTrackerService)
        .SetProtocolVersion(1));

    DEFINE_RPC_PROXY_METHOD(NProto::NTransactionSupervisor, CommitTransaction);
    DEFINE_RPC_PROXY_METHOD(NProto::NTransactionSupervisor, AbortTransaction);
    DEFINE_RPC_PROXY_METHOD(NProto::NTransactionSupervisor, PingTransaction);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHiveClient
} // namespace NYT
