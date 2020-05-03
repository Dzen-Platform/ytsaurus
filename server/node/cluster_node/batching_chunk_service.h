#pragma once

#include "public.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/core/actions/public.h>

#include <yt/core/rpc/public.h>

namespace NYT::NClusterNode {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateBatchingChunkService(
    NElection::TCellId cellId,
    TBatchingChunkServiceConfigPtr serviceConfig,
    NApi::NNative::TMasterConnectionConfigPtr connectionConfig,
    NRpc::IChannelFactoryPtr channelFactory);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClusterNode
