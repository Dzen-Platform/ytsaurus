#include "job_environment.h"
#include "config.h"
#include "private.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/data_node/master_connector.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/tools/tools.h>

#include <yt/core/misc/process.h>
#include <yt/core/misc/proc.h>

#include <util/system/execpath.h>

namespace NYT {
namespace NExecAgent {

using namespace NCGroup;
using namespace NCellNode;
using namespace NConcurrency;
using namespace NYTree;
using namespace NTools;

static const auto& Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

Stroka GetSlotProcessGroup(int slotIndex)
{
    return "slots/" + ToString(slotIndex);
}

////////////////////////////////////////////////////////////////////////////////

class TProcessJobEnvironmentBase
    : public IJobEnvironment
{
public:
    TProcessJobEnvironmentBase(TJobEnvironmentConfigPtr config, const TBootstrap* bootstrap)
        : BasicConfig_(std::move(config))
        , Bootstrap_(bootstrap)
    { }

    virtual TFuture<void> RunJobProxy(
        int slotIndex,
        const Stroka& workingDirectory,
        const TJobId& jobId,
        const TOperationId& operationId) override
    {
        ValidateEnabled();

        try {
            // JobProxy is the same ytserver binary as we are.
            auto jobProxy = New<TProcess>(GetExecPath());

            jobProxy->AddArguments({
                "--job-proxy",
                "--config", ProxyConfigFileName,
                "--job-id", ToString(jobId),
                "--operation-id", ToString(operationId),
                "--working-dir", workingDirectory
            });

            AddArguments(jobProxy, slotIndex);

            LOG_INFO("Spawning a job proxy (SlotIndex: %v, JobId: %v, OperationId: %v, WorkingDirectory: %v)", 
                slotIndex, 
                jobId,
                operationId,
                workingDirectory);

            // Make forks outside controller thread.
            auto result = BIND([=] () {
                    return jobProxy->Spawn();
                })
                .AsyncVia(ActionQueue_->GetInvoker())
                .Run();

            JobProxyProcesses_[slotIndex] = jobProxy;
            return result;

        } catch (const std::exception& ex) {
            auto error = TError("Failed to spawn job proxy") << ex;
            Disable(error);

            THROW_ERROR error;
        }
    }

    virtual bool IsEnabled() const override
    {
        return Enabled_;
    }

protected:
    const TJobEnvironmentConfigPtr BasicConfig_;
    yhash_map<int, TProcessPtr> JobProxyProcesses_;
    const TBootstrap* const Bootstrap_;
    TActionQueuePtr ActionQueue_ = New<TActionQueue>("JobEnvironment");

    bool Enabled_ = true;

    void ValidateEnabled() const
    {
        if (!Enabled_) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::JobEnvironmentDisabled, 
                "Job environment %Qlv is disabled", 
                BasicConfig_->Type);
        }
    }

    void Disable(const TError& error)
    {
        if (!Enabled_)
            return;

        Enabled_ = false;

        auto alert = TError("Job environment is disabled") << error;

        LOG_ERROR(alert);

        auto masterConnector = Bootstrap_->GetMasterConnector();
        masterConnector->RegisterAlert(alert);
    }

    virtual void AddArguments(TProcessPtr process, int slotIndex)
    { }
};

////////////////////////////////////////////////////////////////////////////////

class TCGroupJobEnvironment
    : public TProcessJobEnvironmentBase
{
public:
    TCGroupJobEnvironment(TCGroupJobEnvironmentConfigPtr config, const TBootstrap* bootstrap)
        : TProcessJobEnvironmentBase(config, bootstrap)
        , Config_(std::move(config))
    {
        if (!HasRootPermissions()) {
            auto error = TError("Failed to initialize \"cgroup\" job environment: root permissions required");
            Disable(error);
        }
    }

    virtual void CleanProcesses(int slotIndex) override
    {
        ValidateEnabled();

        // Kill all processes via freezer.
        auto error = WaitFor(BIND([=] () {
                TNonOwningCGroup freezer("freezer", GetSlotProcessGroup(slotIndex));
                freezer.EnsureExistance();
                RunKiller(freezer.GetFullPath());
                freezer.Unlock();
            })
            .AsyncVia(ActionQueue_->GetInvoker())
            .Run());

        if (!error.IsOK()) {
            auto wrapperError = TError("Failed to kill processes in freezer process group (SlotIndex: %v)",
                slotIndex) << error;

            Disable(wrapperError);
            THROW_ERROR wrapperError;
        }

        // Remove all supported cgroups.
        error = WaitFor(BIND([=] () {
                for (const auto& path : GetCGroupPaths(slotIndex)) {
                    TNonOwningCGroup group(path);
                    group.RemoveRecursive();
                }
            })
            .AsyncVia(ActionQueue_->GetInvoker())
            .Run());

        if (!error.IsOK()) {
            auto wrapperError = TError("Failed to clean up cgroups (SlotIndex: %v)",
                slotIndex) << error;
            Disable(wrapperError);
            THROW_ERROR wrapperError;
        }

        // Drop reference to a process if there was any.
        JobProxyProcesses_.erase(slotIndex);
    }

    virtual int GetUserId(int slotIndex) const override
    {
        return Config_->StartUid + slotIndex;
    }

private:
    const TCGroupJobEnvironmentConfigPtr Config_;

    virtual void AddArguments(TProcessPtr process, int slotIndex) override
    {
        for (const auto& path : GetCGroupPaths(slotIndex)) {
            process->AddArguments({"--cgroup", path});
        }
    }

    std::vector<Stroka> GetCGroupPaths(int slotIndex) const
    {
        std::vector<Stroka> result;
        auto subgroupName = GetSlotProcessGroup(slotIndex);

        // Freezer is always implicitly supported.
        TNonOwningCGroup freezer("freezer", subgroupName);
        result.push_back(freezer.GetFullPath());

        for (const auto& type : Config_->SupportedCGroups) {
            TNonOwningCGroup group(type, subgroupName);
            result.push_back(group.GetFullPath());
        }

        return result;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TSimpleJobEnvironment
    : public TProcessJobEnvironmentBase
{
public:
    TSimpleJobEnvironment(TSimpleJobEnvironmentConfigPtr config, const TBootstrap* bootstrap)
        : TProcessJobEnvironmentBase(config, bootstrap)
        , Config_(std::move(config))
    {
        if (!HasRootPermissions_ && Config_->EnforceJobControl) {
            auto error = TError("Failed to initialize \"simple\" job environment: "
                "\"enforce_job_control\" option set, but no root permissions provided");
            Disable(error);
        }
    }

    virtual void CleanProcesses(int slotIndex) override
    {
        ValidateEnabled();

        try {
            auto jobProxy = JobProxyProcesses_[slotIndex];
            if (jobProxy) {
                jobProxy->Kill(SIGKILL);

                // Drop reference to a process if there were any.
                JobProxyProcesses_.erase(slotIndex);
            }

            if (HasRootPermissions_) {
                RunTool<TKillAllByUidTool>(GetUserId(slotIndex));
            }
        } catch (const std::exception& ex) {
            auto error = TError("Failed to clean processes (SlotIndex: %v)",
                slotIndex) << ex;
            Disable(error);
            THROW_ERROR error;
        }
    }

    virtual int GetUserId(int slotIndex) const override
    {
        return HasRootPermissions_
            ? Config_->StartUid + slotIndex
            : ::getuid();
    }

private:
    const TSimpleJobEnvironmentConfigPtr Config_;
    const bool HasRootPermissions_ = HasRootPermissions();
};

////////////////////////////////////////////////////////////////////////////////

IJobEnvironmentPtr CreateJobEnvironment(INodePtr configNode, const TBootstrap* bootstrap)
{
    auto config = ConvertTo<TJobEnvironmentConfigPtr>(configNode);
    switch (config->Type) {
        case EJobEnvironmentType::Simple: {
            auto simpleConfig = ConvertTo<TSimpleJobEnvironmentConfigPtr>(configNode);
            return New<TSimpleJobEnvironment>(
                simpleConfig,
                bootstrap);
        }

        case EJobEnvironmentType::Cgroups: {
            auto cgroupConfig = ConvertTo<TCGroupJobEnvironmentConfigPtr>(configNode);
            return New<TCGroupJobEnvironment>(
                cgroupConfig,
                bootstrap);
        }

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
