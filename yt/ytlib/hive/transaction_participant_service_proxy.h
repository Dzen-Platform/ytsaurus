#pragma once

#include "public.h"

#include <yt/ytlib/hive/transaction_participant_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NHiveClient {

////////////////////////////////////////////////////////////////////////////////

class TTransactionParticipantServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TTransactionParticipantServiceProxy, RPC_PROXY_DESC(TransactionParticipantService)
        .SetProtocolVersion(0));

    DEFINE_RPC_PROXY_METHOD(NProto::NTransactionParticipant, PrepareTransaction);
    DEFINE_RPC_PROXY_METHOD(NProto::NTransactionParticipant, CommitTransaction);
    DEFINE_RPC_PROXY_METHOD(NProto::NTransactionParticipant, AbortTransaction);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHiveClient
} // namespace NYT
