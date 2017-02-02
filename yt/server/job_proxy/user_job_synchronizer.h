#pragma once

#include "public.h"

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/bus/public.h>

#include <yt/core/logging/public.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////

//! Represents the "client side", where satellite can send notifications.
struct IUserJobSynchronizerClient
    : public virtual TRefCounted
{
    virtual void NotifyJobSatellitePrepared() = 0;
    virtual void NotifyUserJobFinished(const TError& error) = 0;
    virtual void NotifyExecutorPrepared() = 0;
};

DEFINE_REFCOUNTED_TYPE(IUserJobSynchronizerClient)

////////////////////////////////////////////////////////////////////

//! Represents the "server side", where we can wait for client.
struct IUserJobSynchronizer
    :  public virtual TRefCounted
{
    virtual void Wait() = 0;
    virtual TError GetUserProcessStatus() const = 0;
    virtual void CancelWait() = 0;
};

DEFINE_REFCOUNTED_TYPE(IUserJobSynchronizer)

////////////////////////////////////////////////////////////////////

class TUserJobSynchronizer
    : public IUserJobSynchronizerClient
    , public IUserJobSynchronizer
{
public:
    virtual void NotifyJobSatellitePrepared() override;
    virtual void NotifyExecutorPrepared() override;
    virtual void NotifyUserJobFinished(const TError &error) override;
    virtual void Wait() override;
    virtual TError GetUserProcessStatus() const override;
    virtual void CancelWait() override;

private:
    TPromise<void> JobSatellitePreparedPromise_ = NewPromise<void>();
    TPromise<void> UserJobFinishedPromise_ = NewPromise<void>();
    TPromise<void> ExecutorPreparedPromise_ = NewPromise<void>();
};

DEFINE_REFCOUNTED_TYPE(TUserJobSynchronizer)

////////////////////////////////////////////////////////////////////////////////

IUserJobSynchronizerClientPtr CreateUserJobSynchronizerClient(NBus::TTcpBusClientConfigPtr config);
NRpc::IServicePtr CreateUserJobSynchronizerService(
    const NLogging::TLogger& logger,
    IUserJobSynchronizerClientPtr jobControl,
    IInvokerPtr controlInvoker);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
