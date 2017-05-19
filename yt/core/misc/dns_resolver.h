#pragma once

#include <yt/core/actions/future.h>
#include <yt/core/actions/invoker.h>

#include <yt/core/misc/address.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TDnsResolver
{
public:
    TDnsResolver(
        int retries,
        TDuration resolveTimeout,
        TDuration maxResolveTimeout,
        TDuration warningTimeout);
    ~TDnsResolver();

    void Start();
    void Stop();

    // Kindly note that returned future is set in special resolver thread
    // which does not support fibers. So please use Via/AsyncVia when
    // using this method.
    TFuture<TNetworkAddress> ResolveName(
        Stroka hostName,
        bool enableIPv4,
        bool enableIPv6);

private:
    class TImpl;
    const std::unique_ptr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

