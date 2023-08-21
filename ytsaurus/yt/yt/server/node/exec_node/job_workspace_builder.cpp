#include "job_workspace_builder.h"

#include "job_gpu_checker.h"
#include "job_directory_manager.h"

#include <yt/yt/server/lib/exec_node/helpers.h>

#include <yt/yt/core/actions/cancelable_context.h>

#include <yt/yt/core/concurrency/thread_affinity.h>
#include <yt/yt/core/concurrency/delayed_executor.h>

#include <yt/yt/core/misc/fs.h>

namespace NYT::NExecNode
{

using namespace NConcurrency;
using namespace NContainers;
using namespace NJobAgent;
using namespace NFS;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ExecNodeLogger;
static const TString MountSuffix = "mount";

////////////////////////////////////////////////////////////////////////////////

TJobWorkspaceBuilder::TJobWorkspaceBuilder(
    IInvokerPtr invoker,
    TJobWorkspaceBuildSettings settings,
    IJobDirectoryManagerPtr directoryManager)
    : Invoker_(std::move(invoker))
    , Settings_(std::move(settings))
    , DirectoryManager_(std::move(directoryManager))
{
    YT_VERIFY(Settings_.Slot);
    YT_VERIFY(Settings_.Job);
    YT_VERIFY(DirectoryManager_);

    if (Settings_.NeedGpuCheck) {
        YT_VERIFY(Settings_.GpuCheckBinaryPath);
        YT_VERIFY(Settings_.GpuCheckBinaryArgs);
    }
}

template<TFuture<void>(TJobWorkspaceBuilder::*Method)()>
TFuture<void> TJobWorkspaceBuilder::GuardedAction()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    switch (Settings_.Job->GetPhase()) {
        case EJobPhase::WaitingAbort:
        case EJobPhase::Cleanup:
        case EJobPhase::Finished:
            return VoidFuture;

        case EJobPhase::Created:
            YT_VERIFY(Settings_.Job->GetState() == EJobState::Waiting);
            break;

        default:
            YT_VERIFY(Settings_.Job->GetState() == EJobState::Running);
            break;
    }

    TForbidContextSwitchGuard contextSwitchGuard;
    return (*this.*Method)();
}

template<TFuture<void>(TJobWorkspaceBuilder::*Method)()>
TCallback<TFuture<void>()> TJobWorkspaceBuilder::MakeStep()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return BIND([=, this, this_ = MakeStrong(this)] () {
        return GuardedAction<Method>();
    }).AsyncVia(Invoker_);
}

void TJobWorkspaceBuilder::ValidateJobPhase(EJobPhase expectedPhase) const
{
    VERIFY_THREAD_AFFINITY(JobThread);

    auto jobPhase = Settings_.Job->GetPhase();
    if (jobPhase != expectedPhase) {
        THROW_ERROR_EXCEPTION("Unexpected job phase")
            << TErrorAttribute("expected_phase", expectedPhase)
            << TErrorAttribute("actual_phase", jobPhase);
    }
}

void TJobWorkspaceBuilder::SetJobPhase(EJobPhase phase)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    UpdateBuilderPhase_.Fire(phase);
}

void TJobWorkspaceBuilder::UpdateArtifactStatistics(i64 compressedDataSize, bool cacheHit)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    UpdateArtifactStatistics_.Fire(compressedDataSize, cacheHit);
}

TFuture<TJobWorkspaceBuildResult> TJobWorkspaceBuilder::Run()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return BIND(&TJobWorkspaceBuilder::DoPrepareSandboxDirectories, MakeStrong(this))
        .AsyncVia(Invoker_)
        .Run()
        .Apply(MakeStep<&TJobWorkspaceBuilder::DoPrepareRootVolume>())
        .Apply(MakeStep<&TJobWorkspaceBuilder::DoRunSetupCommand>())
        .Apply(MakeStep<&TJobWorkspaceBuilder::DoRunGpuCheckCommand>())
        .Apply(BIND([=, this, this_ = MakeStrong(this)] (const TError& result) {
            ResultHolder_.LastBuildError = result;
            return std::move(ResultHolder_);
        }).AsyncVia(Invoker_));
}

////////////////////////////////////////////////////////////////////////////////

class TSimpleJobWorkspaceBuilder
    : public TJobWorkspaceBuilder
{
public:
    TSimpleJobWorkspaceBuilder(
        IInvokerPtr invoker,
        TJobWorkspaceBuildSettings settings,
        IJobDirectoryManagerPtr directoryManager)
        : TJobWorkspaceBuilder(
            std::move(invoker),
            std::move(settings),
            std::move(directoryManager))
    { }

private:

    void MakeArtifactSymlinks()
    {
        auto slot = Settings_.Slot;

        for (const auto& artifact : Settings_.Artifacts) {
            // Artifact is passed into the job via symlink.
            if (!artifact.BypassArtifactCache && !artifact.CopyFile) {
                YT_VERIFY(artifact.Chunk);

                YT_LOG_INFO(
                    "Making symlink for artifact (FileName: %v, Executable: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());

                auto sandboxPath = slot->GetSandboxPath(artifact.SandboxKind);
                auto symlinkPath =
                    CombinePaths(sandboxPath, artifact.Name);

                WaitFor(slot->MakeLink(
                    Settings_.Job->GetId(),
                    artifact.Name,
                    artifact.SandboxKind,
                    artifact.Chunk->GetFileName(),
                    symlinkPath,
                    artifact.Executable))
                    .ThrowOnError();
            } else {
                YT_VERIFY(artifact.SandboxKind == ESandboxKind::User);
            }
        }
    }

    TRootFS MakeWritableRootFS()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(ResultHolder_.RootVolume);

        std::vector<TBind> binds = Settings_.Binds;

        for (const auto& bind : ResultHolder_.RootBinds) {
            binds.push_back(bind);
        }

        return TRootFS {
            .RootPath = ResultHolder_.RootVolume->GetPath(),
            .IsRootReadOnly = false,
            .Binds = binds
        };
    }

    TFuture<void> DoPrepareSandboxDirectories()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::DownloadingArtifacts);
        SetJobPhase(EJobPhase::PreparingSandboxDirectories);

        YT_LOG_INFO("Started preparing sandbox directories");

        auto slot = Settings_.Slot;

        ResultHolder_.TmpfsPaths = WaitFor(slot->PrepareSandboxDirectories(Settings_.UserSandboxOptions))
            .ValueOrThrow();

        MakeArtifactSymlinks();

        YT_LOG_INFO("Finished preparing sandbox directories");

        return VoidFuture;
    }

    TFuture<void> DoPrepareRootVolume()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::PreparingSandboxDirectories);
        SetJobPhase(EJobPhase::PreparingRootVolume);

        return VoidFuture;
    }

    TFuture<void> DoRunSetupCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::PreparingRootVolume);
        SetJobPhase(EJobPhase::RunningSetupCommands);

        return VoidFuture;
    }

    TFuture<void> DoRunGpuCheckCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::RunningSetupCommands);
        SetJobPhase(EJobPhase::RunningGpuCheckCommand);

        return VoidFuture;
    }
};

////////////////////////////////////////////////////////////////////////////////

TJobWorkspaceBuilderPtr CreateSimpleJobWorkspaceBuilder(
    IInvokerPtr invoker,
    TJobWorkspaceBuildSettings settings,
    IJobDirectoryManagerPtr directoryManager)
{
    return New<TSimpleJobWorkspaceBuilder>(
        std::move(invoker),
        std::move(settings),
        std::move(directoryManager));
}

////////////////////////////////////////////////////////////////////////////////

#ifdef _linux_

class TPortoJobWorkspaceBuilder
    : public TJobWorkspaceBuilder
{
public:
    TPortoJobWorkspaceBuilder(
        IInvokerPtr invoker,
        TJobWorkspaceBuildSettings settings,
        IJobDirectoryManagerPtr directoryManager)
        : TJobWorkspaceBuilder(
            std::move(invoker),
            std::move(settings),
            std::move(directoryManager))
    { }

private:

    void MakeArtifactSymlinks()
    {
        auto slot = Settings_.Slot;

        for (const auto& artifact : Settings_.Artifacts) {
            // Artifact is passed into the job via symlink.
            if (!artifact.BypassArtifactCache && !artifact.CopyFile) {
                YT_VERIFY(artifact.Chunk);

                YT_LOG_INFO(
                    "Making symlink for artifact (FileName: %v, Executable: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());

                auto sandboxPath = slot->GetSandboxPath(artifact.SandboxKind);
                auto symlinkPath =
                    CombinePaths(sandboxPath, artifact.Name);

                WaitFor(slot->MakeLink(
                    Settings_.Job->GetId(),
                    artifact.Name,
                    artifact.SandboxKind,
                    artifact.Chunk->GetFileName(),
                    symlinkPath,
                    artifact.Executable))
                    .ThrowOnError();
            } else {
                YT_VERIFY(artifact.SandboxKind == ESandboxKind::User);
            }
        }
    }

    void SetArtifactPermissions()
    {
        for (const auto& artifact : Settings_.Artifacts) {
            if (!artifact.BypassArtifactCache && !artifact.CopyFile) {
                YT_VERIFY(artifact.Chunk);

                int permissions = artifact.Executable ? 0755 : 0644;

                YT_LOG_INFO(
                    "Set permissions for artifact (FileName: %v, Permissions: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    permissions,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());

                SetPermissions(
                    artifact.Chunk->GetFileName(),
                    permissions);
            } else {
                YT_VERIFY(artifact.SandboxKind == ESandboxKind::User);
            }
        }
    }

    TFuture<void> DoPrepareSandboxDirectories()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::DownloadingArtifacts);
        SetJobPhase(EJobPhase::PreparingSandboxDirectories);

        YT_LOG_INFO("Started preparing sandbox directories");

        auto slot = Settings_.Slot;
        ResultHolder_.TmpfsPaths = WaitFor(slot->PrepareSandboxDirectories(Settings_.UserSandboxOptions))
            .ValueOrThrow();

        if (Settings_.LayerArtifactKeys.empty() || !Settings_.UserSandboxOptions.EnableArtifactBinds) {
            MakeArtifactSymlinks();
        } else {
            SetArtifactPermissions();
        }

        YT_LOG_INFO("Finished preparing sandbox directories");

        return VoidFuture;
    }

    TRootFS MakeWritableRootFS()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(ResultHolder_.RootVolume);

        auto binds = Settings_.Binds;

        for (const auto& bind : ResultHolder_.RootBinds) {
            binds.push_back(bind);
        }

        return TRootFS{
            .RootPath = ResultHolder_.RootVolume->GetPath(),
            .IsRootReadOnly = false,
            .Binds = binds
        };
    }

    TFuture<void> DoPrepareRootVolume()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::PreparingSandboxDirectories);
        SetJobPhase(EJobPhase::PreparingRootVolume);

        auto slot = Settings_.Slot;
        auto layerArtifactKeys = Settings_.LayerArtifactKeys;

        if (!layerArtifactKeys.empty()) {
            VolumePrepareStartTime_ = TInstant::Now();
            UpdateTimers_.Fire(MakeStrong(this));

            YT_LOG_INFO("Preparing root volume (LayerCount: %v)", layerArtifactKeys.size());

            for (const auto& layer : layerArtifactKeys) {
                i64 layerSize = layer.GetCompressedDataSize();
                UpdateArtifactStatistics(layerSize, slot->IsLayerCached(layer));
            }

            return slot->PrepareRootVolume(
                layerArtifactKeys,
                Settings_.ArtifactDownloadOptions,
                Settings_.UserSandboxOptions)
                .Apply(BIND([=, this, this_ = MakeStrong(this)] (const TErrorOr<IVolumePtr>& volumeOrError) {
                    if (!volumeOrError.IsOK()) {
                        THROW_ERROR_EXCEPTION(TError(EErrorCode::RootVolumePreparationFailed, "Failed to prepare artifacts")
                            << volumeOrError);
                    }

                    VolumePrepareFinishTime_ = TInstant::Now();
                    UpdateTimers_.Fire(MakeStrong(this));
                    ResultHolder_.RootVolume = volumeOrError.Value();
                }));
        } else {
            return VoidFuture;
        }
    }

    TFuture<void> DoRunSetupCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::PreparingRootVolume);
        SetJobPhase(EJobPhase::RunningSetupCommands);

        if (Settings_.LayerArtifactKeys.empty()) {
            return VoidFuture;
        }

        auto slot = Settings_.Slot;

        auto commands = Settings_.SetupCommands;
        ResultHolder_.SetupCommandCount = commands.size();

        if (commands.empty()) {
            return VoidFuture;
        }

        YT_LOG_INFO("Running setup commands");

        return slot->RunSetupCommands(
            Settings_.Job->GetId(),
            commands,
            MakeWritableRootFS(),
            Settings_.CommandUser,
            /*devices*/ std::nullopt,
            /*startIndex*/ 0);
    }

    TFuture<void> DoRunGpuCheckCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::RunningSetupCommands);
        SetJobPhase(EJobPhase::RunningGpuCheckCommand);

        if (Settings_.NeedGpuCheck) {
            TJobGpuCheckerSettings settings {
                .Slot = Settings_.Slot,
                .Job = Settings_.Job,
                .RootFs = MakeWritableRootFS(),
                .CommandUser = Settings_.CommandUser,

                .GpuCheckBinaryPath = *Settings_.GpuCheckBinaryPath,
                .GpuCheckBinaryArgs = *Settings_.GpuCheckBinaryArgs,
                .GpuCheckType = Settings_.GpuCheckType,
                .CurrentStartIndex = ResultHolder_.SetupCommandCount,
                .TestExtraGpuCheckCommandFailure = Settings_.TestExtraGpuCheckCommandFailure,
                .GpuDevices = Settings_.GpuDevices
            };

            auto checker = New<TJobGpuChecker>(std::move(settings));

            checker->SubscribeRunCheck(BIND_NO_PROPAGATE([=, this, this_ = MakeStrong(this)] () {
                GpuCheckStartTime_ = TInstant::Now();
                UpdateTimers_.Fire(MakeStrong(this));
            }));

            checker->SubscribeRunCheck(BIND_NO_PROPAGATE([=, this, this_ = MakeStrong(this)] () {
                GpuCheckFinishTime_ = TInstant::Now();
                UpdateTimers_.Fire(MakeStrong(this));
            }));

            return BIND(&TJobGpuChecker::RunGpuCheck, checker)
                .AsyncVia(Invoker_)
                .Run()
                .Apply(BIND([=, this, this_ = MakeStrong(this)] (const TError& result) {
                    ValidateJobPhase(EJobPhase::RunningGpuCheckCommand);
                    if (!result.IsOK()) {
                        auto checkError = TError(EErrorCode::GpuCheckCommandFailed, "Preliminary GPU check command failed")
                            << result;
                        THROW_ERROR checkError;
                    }
                }).AsyncVia(Invoker_));
        } else {
            return VoidFuture;
        }
    }
};

TJobWorkspaceBuilderPtr CreatePortoJobWorkspaceBuilder(
    IInvokerPtr invoker,
    TJobWorkspaceBuildSettings settings,
    IJobDirectoryManagerPtr directoryManager)
{
    return New<TPortoJobWorkspaceBuilder>(
        std::move(invoker),
        std::move(settings),
        std::move(directoryManager));
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
