#pragma once

#include "public.h"

#include <yt/core/rpc/public.h>

namespace NYT::NTransactionClient {

////////////////////////////////////////////////////////////////////////////////

NRpc::IChannelPtr CreateTimestampProviderChannel(
    TRemoteTimestampProviderConfigPtr config,
    NRpc::IChannelFactoryPtr channelFactory);

NRpc::IChannelPtr CreateTimestampProviderChannelFromAddresses(
    TRemoteTimestampProviderConfigPtr config,
    NRpc::IChannelFactoryPtr channelFactory,
    const std::vector<TString>& addresses);

ITimestampProviderPtr CreateBatchingTimestampProvider(
    ITimestampProviderPtr underlying,
    TDuration updatePeriod,
    TDuration batchPeriod);

ITimestampProviderPtr CreateRemoteTimestampProvider(
    TRemoteTimestampProviderConfigPtr config,
    NRpc::IChannelPtr channel);

ITimestampProviderPtr CreateBatchingRemoteTimestampProvider(
    TBatchingRemoteTimestampProviderConfigPtr config,
    NRpc::IChannelPtr channel);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionClient
