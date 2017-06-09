#include "container_manager.h"
#include "instance.h"
#include "porto_executor.h"
#include "private.h"

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/ytree/convert.h>

#include <yt/core/logging/log.h>

namespace NYT {
namespace NContainers {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static NLogging::TLogger& Logger = ContainersLogger;

////////////////////////////////////////////////////////////////////////////////

static Stroka GetRelativeName(IPortoExecutorPtr executor)
{
    auto properties = WaitFor(executor->GetProperties(
        "self",
        std::vector<Stroka>{"absolute_name", "absolute_namespace"}))
            .ValueOrThrow();

    auto absoluteName = properties.at("absolute_name")
        .ValueOrThrow();
    auto absoluteNameSpace = properties.at("absolute_namespace")
        .ValueOrThrow();

    // Container without porto_namespace:
    // absolute_name = /porto/foo
    // absolute_namespace = /porto/
    //
    // Container with porto_namespace:
    // absolute_name = /porto/foo
    // absolute_namespace = /porto/foo/
    //
    // Root container (host):
    // absolute_name = /
    // absolute_namespace = /porto/
    //
    // root container is a special case - return empty string

    if (absoluteNameSpace.size() > absoluteName.size()) {
        return {};
    }
    return absoluteName.substr(absoluteNameSpace.size()) + "/";
}

////////////////////////////////////////////////////////////////////////////////

class TPortoManager
    : public IContainerManager
{
public:
    virtual IInstancePtr CreateInstance() override
    {
        return CreatePortoInstance(
            RelativeName_ + Prefix_ + '_' + ToString(InstanceId_++),
            Executor_);
    }

    virtual IInstancePtr GetSelfInstance() override
    {
        return GetSelfPortoInstance(Executor_);
    }

    virtual TFuture<std::vector<Stroka>> GetInstanceNames() const override
    {
        return Executor_->ListContainers();
    }

    static IContainerManagerPtr Create(
        const Stroka& prefix,
        TCallback<void(const TError&)> errorHandler,
        const TPortoManagerConfig& portoManagerConfig)
    {
        auto executor = CreatePortoExecutor(
            portoManagerConfig.RetryTime,
            portoManagerConfig.PollPeriod);
        executor->SubscribeFailed(errorHandler);

        auto relativeName = GetRelativeName(executor);

        auto manager = New<TPortoManager>(
            prefix,
            relativeName,
            portoManagerConfig,
            executor);
        manager->CleanContainers();
        return manager;
    }

private:
    const Stroka Prefix_;
    const Stroka RelativeName_;
    const TPortoManagerConfig PortoManagerConfig_;

    mutable IPortoExecutorPtr Executor_;
    std::atomic<ui64> InstanceId_ = {0};

    TPortoManager(
        const Stroka& prefix,
        const Stroka& relativeName,
        const TPortoManagerConfig& portoManagerConfig,
        IPortoExecutorPtr executor)
        : Prefix_(prefix)
        , RelativeName_(relativeName)
        , PortoManagerConfig_(portoManagerConfig)
        , Executor_(executor)
    {
        LOG_DEBUG("Porto manager initialized (Prefix: %v, RelativeName: %v)",
            Prefix_,
            RelativeName_);
    }

    Stroka GetState(const Stroka& name) const
    {
        auto state = WaitFor(Executor_->GetProperties(name, std::vector<Stroka>{"state"}))
            .ValueOrThrow();
        return state.at("state")
            .ValueOrThrow();
    }

    virtual TFuture<void> Destroy(const Stroka& name)
    {
        return Executor_->DestroyContainer(name);
    }

    void CleanContainers()
    {
        if (PortoManagerConfig_.CleanMode == ECleanMode::None) {
            return;
        }

        const auto containers = WaitFor(GetInstanceNames())
            .ValueOrThrow();
        LOG_DEBUG("Cleaning requested (Prefix: %v, Containers: %v, RelativeName: %v)",
            Prefix_,
            containers,
            RelativeName_);

        std::vector<TFuture<void>> actions;
        for (const auto& name : containers) {
            if (name == "/") {
                continue;
            }
            if (!name.StartsWith(RelativeName_ + Prefix_)) {
                continue;
            }
            if (PortoManagerConfig_.CleanMode == ECleanMode::Dead) {
                auto state = GetState(name);
                if (state != "dead") {
                    continue;
                }
            }
            LOG_DEBUG("Cleaning (Container: %v)", name);
            actions.push_back(Destroy(name));
        }
        WaitFor(Combine(actions))
            .ThrowOnError();
    }

    DECLARE_NEW_FRIEND();
};

////////////////////////////////////////////////////////////////////////////////

IContainerManagerPtr CreatePortoManager(
    const Stroka& prefix,
    TCallback<void(const TError&)> errorHandler,
    const TPortoManagerConfig& portoManagerConfig)
{
    return TPortoManager::Create(prefix, errorHandler, portoManagerConfig);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NContainers
} // namespace NYT
