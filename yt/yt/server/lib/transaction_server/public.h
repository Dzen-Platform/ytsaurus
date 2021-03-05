#pragma once

#include <yt/yt/server/lib/hive/public.h>

#include <yt/yt/ytlib/object_client/public.h>

#include <yt/yt/ytlib/transaction_client/public.h>

namespace NYT::NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

using NObjectClient::TTransactionId;
using NObjectClient::NullTransactionId;

using NTransactionClient::TTimestamp;
using NTransactionClient::NullTimestamp;
using NTransactionClient::TTransactionActionData;

using NHiveServer::ETransactionState;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer
