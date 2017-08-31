#include "resource_controller.h"

#include <yt/server/exec_agent/config.h>

#include <yt/server/containers/container_manager.h>
#include <yt/server/containers/instance.h>

#include <yt/server/misc/process.h>

#include <yt/core/logging/log_manager.h>

#include <yt/core/ytree/convert.h>

namespace NYT {
namespace NJobProxy {

using namespace NContainers;
using namespace NCGroup;
using namespace NExecAgent;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

// Option cpu.share is limited to [2, 1024], see http://git.kernel.org/cgit/linux/kernel/git/tip/tip.git/tree/kernel/sched/sched.h#n279
// To overcome this limitation we consider one cpu_limit unit as ten cpu.shares units.
static constexpr int CpuShareMultiplier = 10;

static NLogging::TLogger Logger("ResourceController");

////////////////////////////////////////////////////////////////////////////////

class TCGroupResourceController
    : public IResourceController
{
public:
    TCGroupResourceController(
        TCGroupJobEnvironmentConfigPtr config,
        const TString& path = TString())
        : CGroupsConfig_(config)
        , CGroups_(path)
        , Path_(path)
    { }

    virtual TCpuStatistics GetCpuStatistics() const override
    {
        if (CGroupsConfig_->IsCGroupSupported(TCpuAccounting::Name)) {
            return CGroups_.CpuAccounting.GetStatistics();
        }
        THROW_ERROR_EXCEPTION("Cpu accounting cgroup is not supported");
    }

    virtual TBlockIOStatistics GetBlockIOStatistics() const override
    {
        if (CGroupsConfig_->IsCGroupSupported(TBlockIO::Name)) {
            return CGroups_.BlockIO.GetStatistics();
        }
        THROW_ERROR_EXCEPTION("Block io cgroup is not supported");
    }

    virtual TMemoryStatistics GetMemoryStatistics() const override
    {
        if (CGroupsConfig_->IsCGroupSupported(TMemory::Name)) {
            return CGroups_.Memory.GetStatistics();
        }
        THROW_ERROR_EXCEPTION("Memory cgroup is not supported");
    }

    virtual i64 GetMaxMemoryUsage() const override
    {
        if (CGroupsConfig_->IsCGroupSupported(TMemory::Name)) {
            return  CGroups_.Memory.GetMaxMemoryUsage();
        }
        THROW_ERROR_EXCEPTION("Memory cgroup is not supported");
    }

    virtual TDuration GetBlockIOWatchdogPeriod() const override
    {
        return CGroupsConfig_->BlockIOWatchdogPeriod;
    }

    virtual void KillAll() override
    {
        try {
            // Kill everything for sanity reasons: main user process completed,
            // but its children may still be alive.
            RunKiller(CGroups_.Freezer.GetFullPath());
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Failed to kill user processes");
        }
    }

    virtual void SetCpuShare(double share) override
    {
        if (CGroupsConfig_->IsCGroupSupported(TCpu::Name)) {
            CGroups_.Cpu.SetShare(share * CpuShareMultiplier);
        }
    }

    virtual void SetIOThrottle(i64 operations) override
    {
        if (CGroupsConfig_->IsCGroupSupported(TBlockIO::Name)) {
            CGroups_.BlockIO.ThrottleOperations(operations);
        }
    }

    virtual IResourceControllerPtr CreateSubcontroller(const TString& name) override
    {
        return New<TCGroupResourceController>(CGroupsConfig_, Path_ + name);
    }

    virtual TProcessBasePtr CreateControlledProcess(const TString& path, const TNullable<TString>& coreDumpHandler) override
    {
        YCHECK(!coreDumpHandler);
        auto process = New<TSimpleProcess>(path, false);
        try {
            {
                CGroups_.Freezer.Create();
                process->AddArguments({"--cgroup", CGroups_.Freezer.GetFullPath()});
            }

            if (CGroupsConfig_->IsCGroupSupported(TCpuAccounting::Name)) {
                CGroups_.CpuAccounting.Create();
                process->AddArguments({"--cgroup", CGroups_.CpuAccounting.GetFullPath()});
                process->AddArguments({"--env", Format("YT_CGROUP_CPUACCT=%v", CGroups_.CpuAccounting.GetFullPath())});
            }

            if (CGroupsConfig_->IsCGroupSupported(TBlockIO::Name)) {
                CGroups_.BlockIO.Create();
                process->AddArguments({"--cgroup", CGroups_.BlockIO.GetFullPath()});
                process->AddArguments({"--env", Format("YT_CGROUP_BLKIO=%v", CGroups_.BlockIO.GetFullPath())});
            }

            if (CGroupsConfig_->IsCGroupSupported(TMemory::Name)) {
                CGroups_.Memory.Create();
                process->AddArguments({"--cgroup", CGroups_.Memory.GetFullPath()});
                process->AddArguments({"--env", Format("YT_CGROUP_MEMORY=%v", CGroups_.Memory.GetFullPath())});
            }
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Failed to create required cgroups");
        }
        return process;
    }

private:
    const TCGroupJobEnvironmentConfigPtr CGroupsConfig_;

    struct TCGroups
    {
        explicit TCGroups(const TString& name)
            : Freezer(name)
            , CpuAccounting(name)
            , BlockIO(name)
            , Memory(name)
            , Cpu(name)
        { }

        TFreezer Freezer;
        TCpuAccounting CpuAccounting;
        TBlockIO BlockIO;
        TMemory Memory;
        TCpu Cpu;
    } CGroups_;

    const TString Path_;
};

////////////////////////////////////////////////////////////////////////////////

template <class ...Args>
static TError CheckErrors(const TUsage& stats, const Args&... args)
{
    std::vector<EStatField> fields = {args...};
    TError error;
    for (const auto& field : fields) {
        if (!stats[field].IsOK()) {
            if (error.IsOK()) {
                error = stats[field];
            } else {
                error << stats[field];
            }
        }
    }
    return error;
}

////////////////////////////////////////////////////////////////////////////////

class TPortoResourceController
    : public IResourceController
{
public:
    static IResourceControllerPtr Create(TPortoJobEnvironmentConfigPtr config)
    {
        auto resourceController = New<TPortoResourceController>(config->BlockIOWatchdogPeriod, config->UseResourceLimits);
        resourceController->Init(config->PortoWaitTime, config->PortoPollPeriod);
        return resourceController;
    }

    virtual TCpuStatistics GetCpuStatistics() const override
    {
        UpdateResourceUsage();
        auto error = CheckErrors(ResourceUsage_,
            EStatField::CpuUsageSystem,
            EStatField::CpuUsageUser);
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Unable to get cpu statistics");
        TCpuStatistics cpuStatistic;
        // porto returns nanosecond
        cpuStatistic.SystemTime = TDuration().MicroSeconds(ResourceUsage_[EStatField::CpuUsageSystem].Value() / 1000);
        cpuStatistic.UserTime = TDuration().MicroSeconds(ResourceUsage_[EStatField::CpuUsageUser].Value() / 1000);
        return cpuStatistic;
    }

    virtual TBlockIOStatistics GetBlockIOStatistics() const override
    {
        UpdateResourceUsage();
        auto error = CheckErrors(ResourceUsage_,
            EStatField::IOReadByte,
            EStatField::IOWriteByte,
            EStatField::IOOperations);
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Unable to get io statistics");
        TBlockIOStatistics blockIOStatistics;
        blockIOStatistics.BytesRead = ResourceUsage_[EStatField::IOReadByte].Value();
        blockIOStatistics.BytesWritten = ResourceUsage_[EStatField::IOWriteByte].Value();
        blockIOStatistics.IOTotal = ResourceUsage_[EStatField::IOOperations].Value();
        return blockIOStatistics;
    }

    virtual TMemoryStatistics GetMemoryStatistics() const override
    {
        UpdateResourceUsage();
        auto error = CheckErrors(ResourceUsage_,
            EStatField::Rss,
            EStatField::MappedFiles,
            EStatField::MajorFaults);
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Unable to get memory statistics");
        TMemoryStatistics memoryStatistics;
        memoryStatistics.Rss = ResourceUsage_[EStatField::Rss].Value();
        memoryStatistics.MappedFile = ResourceUsage_[EStatField::MappedFiles].Value();
        memoryStatistics.MajorPageFaults = ResourceUsage_[EStatField::MajorFaults].Value();
        return memoryStatistics;
    }

    virtual i64 GetMaxMemoryUsage() const override
    {
        UpdateResourceUsage();
        auto error = CheckErrors(ResourceUsage_,
            EStatField::MaxMemoryUsage);
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Unable to get max memory usage");
        return ResourceUsage_[EStatField::MaxMemoryUsage].Value();
    }

    virtual TDuration GetBlockIOWatchdogPeriod() const override
    {
        return BlockIOWatchdogPeriod_;
    }

    virtual void KillAll() override
    {
        // Kill only first process in container,
        // others will be killed automaticaly
        try {
            Container_->Kill(SIGKILL);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Failed to kill user container");
        }
    }

    virtual void SetCpuShare(double share) override
    {
        if (UseResourceLimits_) {
            Container_->SetCpuShare(share);
        }
    }

    virtual void SetIOThrottle(i64 operations) override
    {
        if (UseResourceLimits_) {
            Container_->SetIOThrottle(operations);
        }
    }

    virtual IResourceControllerPtr CreateSubcontroller(const TString& name) override
    {
        auto instance = ContainerManager_->CreateInstance();
        return New<TPortoResourceController>(ContainerManager_, instance, BlockIOWatchdogPeriod_, UseResourceLimits_);
    }

    virtual TProcessBasePtr CreateControlledProcess(const TString& path, const TNullable<TString>& coreDumpHandler) override
    {
        if (coreDumpHandler) {
            LOG_DEBUG("Enable core forwarding for porto container (CoreHandler: %v)",
                coreDumpHandler.Get());
            Container_->SetCoreDumpHandler(coreDumpHandler.Get());
        }
        return New<TPortoProcess>(path, Container_, false);
    }

private:
    IContainerManagerPtr ContainerManager_;
    IInstancePtr Container_;
    mutable TInstant LastUpdateTime_ = TInstant::Zero();
    mutable TUsage ResourceUsage_;

    const TDuration StatUpdatePeriod_;
    const TDuration BlockIOWatchdogPeriod_;
    const bool UseResourceLimits_;

    TPortoResourceController(
        TDuration blockIOWatchdogPeriod,
        bool useResourceLimits)
        : StatUpdatePeriod_(TDuration::MilliSeconds(100))
        , BlockIOWatchdogPeriod_(blockIOWatchdogPeriod)
        , UseResourceLimits_(useResourceLimits)
    { }

    TPortoResourceController(
        IContainerManagerPtr containerManager,
        IInstancePtr instance,
        TDuration blockIOWatchdogPeriod,
        bool useResourceLimits)
        : ContainerManager_(containerManager)
        , Container_(instance)
        , BlockIOWatchdogPeriod_(blockIOWatchdogPeriod)
        , UseResourceLimits_(useResourceLimits)
    { }

    void UpdateResourceUsage() const
    {
        auto now = TInstant::Now();
        if (now > LastUpdateTime_ && now - LastUpdateTime_ > StatUpdatePeriod_) {
            ResourceUsage_ = Container_->GetResourceUsage({
                EStatField::CpuUsageUser,
                EStatField::CpuUsageSystem,
                EStatField::IOReadByte,
                EStatField::IOWriteByte,
                EStatField::IOOperations,
                EStatField::Rss,
                EStatField::MappedFiles,
                EStatField::MajorFaults,
                EStatField::MaxMemoryUsage
            });
            LastUpdateTime_ = now;
        }
    }

    void OnFatalError(const TError& error)
    {
        // We cant abort user job (the reason is we need porto to do it),
        // so we will abort job proxy
        LOG_ERROR(error, "Fatal error during porto polling");
        NLogging::TLogManager::Get()->Shutdown();
        _exit(static_cast<int>(EJobProxyExitCode::PortoManagmentFailed));
    }

    void Init(TDuration waitTime, TDuration pollPeriod)
    {
        auto errorHandler = BIND(&TPortoResourceController::OnFatalError, MakeStrong(this));
        ContainerManager_ = CreatePortoManager(
            "",
            errorHandler,
            { ECleanMode::None,
            waitTime,
            pollPeriod });
        Container_ = ContainerManager_->GetSelfInstance();
        UpdateResourceUsage();
    }

    DECLARE_NEW_FRIEND();
};

////////////////////////////////////////////////////////////////////////////////

IResourceControllerPtr CreateResourceController(NYTree::INodePtr config)
{
    auto environmentConfig = ConvertTo<TJobEnvironmentConfigPtr>(config);
    switch (environmentConfig->Type) {
        case EJobEnvironmentType::Cgroups:
            return New<TCGroupResourceController>(ConvertTo<TCGroupJobEnvironmentConfigPtr>(config));
        case EJobEnvironmentType::Porto:
            return TPortoResourceController::Create(ConvertTo<TPortoJobEnvironmentConfigPtr>(config));
        case EJobEnvironmentType::Simple:
            return nullptr;
        default:
            THROW_ERROR_EXCEPTION("Unable to create resource controller for %Qv environment",
                environmentConfig->Type);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT

