#pragma once

#include "node_directory.h"

#include <yt/core/rpc/channel.h>

namespace NYT {
namespace NNodeTrackerClient {

////////////////////////////////////////////////////////////////////////////////

struct INodeChannelFactory
    : public NYT::NRpc::IChannelFactory
{
    virtual NRpc::IChannelPtr CreateChannel(const TNodeDescriptor& descriptor) = 0;

    virtual NRpc::IChannelPtr CreateChannel(const Stroka& address) = 0;
};

DEFINE_REFCOUNTED_TYPE(INodeChannelFactory)

INodeChannelFactoryPtr CreateNodeChannelFactory(
    NYT::NRpc::IChannelFactoryPtr channelFactory,
    const TNetworkPreferenceList& networks);

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT
