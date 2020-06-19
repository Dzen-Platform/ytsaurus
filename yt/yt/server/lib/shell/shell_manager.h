#pragma once

#include "public.h"

#include <yt/server/lib/containers/public.h>

#include <yt/core/actions/future.h>

#include <yt/core/misc/intrusive_ptr.h>
#include <yt/core/misc/optional.h>

#include <yt/core/yson/string.h>

namespace NYT::NShell {

////////////////////////////////////////////////////////////////////////////////

struct IShellManager
    : public virtual TRefCounted
{
    virtual NYson::TYsonString PollJobShell(const NYson::TYsonString& parameters) = 0;
    virtual void Terminate(const TError& error) = 0;
    virtual TFuture<void> GracefulShutdown(const TError& error) = 0;
};

DEFINE_REFCOUNTED_TYPE(IShellManager)

////////////////////////////////////////////////////////////////////////////////

IShellManagerPtr CreateShellManager(
    NContainers::IPortoExecutorPtr portoExecutor,
    NContainers::IInstancePtr rootInstnace,
    const TString& preparationDir,
    const TString& workingDir,
    std::optional<int> userId,
    std::optional<TString> messageOfTheDay,
    std::vector<TString> environment);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
