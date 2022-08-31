#pragma once

#include "public.h"

#include <yt/yt/server/node/cluster_node/public.h>

#include <yt/yt/core/actions/signal.h>

#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt_proto/yt/client/node_tracker_client/proto/node.pb.h>

namespace NYT::NJobAgent {

////////////////////////////////////////////////////////////////////////////////

class IJobResourceManager
    : public TRefCounted
{
protected:
    class TResourceAcquiringProxy;
    class TImpl;

public:
    virtual void Initialize() = 0;
    
    //! Returns the maximum allowed resource usage.
    virtual NNodeTrackerClient::NProto::TNodeResources GetResourceLimits() const = 0;

    virtual NNodeTrackerClient::NProto::TDiskResources GetDiskResources() const = 0;

    //! Set resource limits overrides.
    virtual void SetResourceLimitsOverrides(const NNodeTrackerClient::NProto::TNodeResourceLimitsOverrides& resourceLimits) = 0;

    virtual double GetCpuToVCpuFactor() const = 0;

    //! Returns resource usage of running jobs.
    virtual NNodeTrackerClient::NProto::TNodeResources GetResourceUsage(bool includeWaiting = false) const = 0;

    //! Compares new usage with resource limits. Detects resource overdraft.
    virtual bool CheckMemoryOverdraft(const NNodeTrackerClient::NProto::TNodeResources& delta) = 0;

    virtual TResourceAcquiringProxy GetResourceAcquiringProxy() = 0;

    static IJobResourceManagerPtr CreateJobResourceManager(NClusterNode::IBootstrapBase* bootstrap);

    DECLARE_INTERFACE_SIGNAL(void(), ResourcesUpdated);
    DECLARE_INTERFACE_SIGNAL(
        void(i64 mapped),
        ReservedMemoryOvercommited);
    
    DECLARE_INTERFACE_SIGNAL(void(), ResourcesReleased);

protected:
    friend TResourceHolder;

    class TResourceAcquiringProxy
    {
    public:
        TResourceAcquiringProxy(IJobResourceManager* resourceManagerImpl);
        TResourceAcquiringProxy(const TResourceAcquiringProxy&) = delete;
        ~TResourceAcquiringProxy();
        
        bool TryAcquireResourcesFor(TResourceHolder* resourceHolder) &;

    private:
        NConcurrency::TForbidContextSwitchGuard Guard_;
        TImpl* const ResourceManagerImpl_;
    };
};

DEFINE_REFCOUNTED_TYPE(IJobResourceManager)

////////////////////////////////////////////////////////////////////////////////

class TResourceHolder
{
public:
    TResourceHolder(
        IJobResourceManager* jobResourceManager,
        NLogging::TLogger logger,
        const NNodeTrackerClient::NProto::TNodeResources& jobResources,
        int portCount);

    TResourceHolder(const TResourceHolder&) = delete;
    TResourceHolder(TResourceHolder&&) = delete;
    ~TResourceHolder();

    void ReleaseResources();

    const std::vector<int>& GetPorts() const noexcept;

    //! Returns resource usage delta.
    NNodeTrackerClient::NProto::TNodeResources SetResourceUsage(
        NNodeTrackerClient::NProto::TNodeResources newResourceUsage);

    const NNodeTrackerClient::NProto::TNodeResources& GetResourceUsage() const noexcept;

    const NLogging::TLogger& GetLogger() const noexcept;

protected:
    NLogging::TLogger Logger;

private:
    friend IJobResourceManager::TResourceAcquiringProxy;
    friend IJobResourceManager::TImpl;

    IJobResourceManager::TImpl* const ResourceManagerImpl_;

    const int PortCount_;
    NNodeTrackerClient::NProto::TNodeResources Resources_;

    std::vector<int> Ports_;

    enum class EResourcesState
    {
        Waiting,
        Acquired,
        Released,
    };

    EResourcesState State_ = EResourcesState::Waiting;

    class TAcquiredResources;
    void AcquireResources(TAcquiredResources&& acquiredResources);
    virtual void OnResourcesAcquired() = 0;   
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent