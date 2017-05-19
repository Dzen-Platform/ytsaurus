#pragma once

#include "public.h"
#include "channel.h"

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

class TStaticChannelFactory
    : public IChannelFactory
{
public:
    TStaticChannelFactoryPtr Add(const Stroka& address, IChannelPtr channel);

    virtual IChannelPtr CreateChannel(const Stroka& address) override;

private:
    yhash<Stroka, IChannelPtr> ChannelMap;

};

DEFINE_REFCOUNTED_TYPE(TStaticChannelFactory)

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
