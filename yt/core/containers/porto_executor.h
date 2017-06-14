#pragma once

#include "public.h"

#include <yt/core/actions/future.h>
#include <yt/core/actions/signal.h>

#if defined(_linux_)
#include <yt/contrib/portoapi/rpc.pb.h>
#include <yt/contrib/portoapi/libporto.hpp>
#endif

namespace NYT {
namespace NContainers {

////////////////////////////////////////////////////////////////////////////////

struct TVolumeID
{
    TString Path;
};

////////////////////////////////////////////////////////////////////////////////

const int ContainerErrorCodeBase = 12000;

#if defined(_linux_)
DEFINE_ENUM(EContainerErrorCode,
    ((InvalidState)((ContainerErrorCodeBase + ::rpc::EError::InvalidState)))
);
#else
DEFINE_ENUM(EContainerErrorCode,
    ((InvalidState)((ContainerErrorCodeBase + 1)))
);
#endif


////////////////////////////////////////////////////////////////////////////////

struct IPortoExecutor
    : public TRefCounted
{
    virtual TFuture<void> CreateContainer(const TString& name) = 0;
    virtual TFuture<void> SetProperty(
        const TString& name,
        const TString& key,
        const TString& value) = 0;
    virtual TFuture<std::map<TString, TErrorOr<TString>>> GetProperties(
        const TString& name,
        const std::vector<TString>& value) = 0;
    virtual TFuture<void> DestroyContainer(const TString& name) = 0;
    virtual TFuture<void> Start(const TString& name) = 0;
    virtual TFuture<void> Kill(const TString& name, int signal) = 0;
    virtual TFuture<std::vector<TString>> ListContainers() = 0;
    // Starts polling a given container, returns future with exit code of finished process.
    virtual TFuture<int> AsyncPoll(const TString& name) = 0;
    virtual TFuture<TVolumeID> CreateVolume(
        const TString& path,
        const std::map<TString, TString>& properties) = 0;
    virtual TFuture<void> LinkVolume(
        const TString& path,
        const TString& name) = 0;
    virtual TFuture<void> UnlinkVolume(
        const TString& path,
        const TString& name) = 0;
    virtual TFuture<std::vector<Porto::Volume>> ListVolumes() = 0;
    DECLARE_INTERFACE_SIGNAL(void(const TError&), Failed)
};

DEFINE_REFCOUNTED_TYPE(IPortoExecutor)

////////////////////////////////////////////////////////////////////////////////

#if defined(_linux_)
IPortoExecutorPtr CreatePortoExecutor(
    TDuration retryTime = TDuration::Seconds(10),
    TDuration pollPeriod = TDuration::MilliSeconds(100));
#else
inline IPortoExecutorPtr CreatePortoExecutor(
    TDuration retryTime = TDuration::Seconds(10),
    TDuration pollPeriod = TDuration::MilliSeconds(100))
{
    Y_UNUSED(retryTime);
    Y_UNUSED(pollPeriod);
    Y_UNIMPLEMENTED();
    return nullptr;
}
#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NContainers
} // namespce NYT
