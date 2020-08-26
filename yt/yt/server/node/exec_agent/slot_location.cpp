#include "slot_location.h"
#include "slot_manager.h"
#include "private.h"
#include "job_directory_manager.h"

#include <yt/server/lib/exec_agent/config.h>

#include <yt/server/node/cluster_node/bootstrap.h>
#include <yt/server/node/cluster_node/config.h>

#include <yt/server/node/data_node/master_connector.h>

#include <yt/server/lib/misc/disk_health_checker.h>

#include <yt/ytlib/scheduler/proto/job.pb.h>

#include <yt/ytlib/tools/tools.h>
#include <yt/ytlib/tools/proc.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/fs.h>
#include <yt/core/misc/singleton.h>
#include <yt/core/misc/proc.h>

#include <yt/core/yson/writer.h>

#include <yt/core/ytree/convert.h>

#include <util/system/fs.h>

#include <util/folder/path.h>

namespace NYT::NExecAgent {

using namespace NConcurrency;
using namespace NTools;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TSlotLocation::TSlotLocation(
    TSlotLocationConfigPtr config,
    NClusterNode::TBootstrap* bootstrap,
    const TString& id,
    IJobDirectoryManagerPtr jobDirectoryManager,
    bool enableTmpfs,
    int slotCount,
    std::function<int(int)> slotIndexToUserId)
    : TDiskLocation(config, id, ExecAgentLogger)
    , Config_(std::move(config))
    , Bootstrap_(bootstrap)
    , JobDirectoryManager_(std::move(jobDirectoryManager))
    , EnableTmpfs_(enableTmpfs)
    , SlotCount_(slotCount)
    , SlotIndexToUserId_(slotIndexToUserId)
    , HeavyLocationQueue_(New<TActionQueue>(Format("HeavyIO:%v", id)))
    , LightLocationQueue_(New<TActionQueue>(Format("LightIO:%v", id)))
    , HeavyInvoker_(HeavyLocationQueue_->GetInvoker())
    , LightInvoker_(LightLocationQueue_->GetInvoker())
    , HealthChecker_(New<TDiskHealthChecker>(
        bootstrap->GetConfig()->DataNode->DiskHealthChecker,
        Config_->Path,
        HeavyInvoker_,
        Logger))
    , DiskResourcesUpdateExecutor_(New<TPeriodicExecutor>(
        HeavyInvoker_,
        BIND(&TSlotLocation::UpdateDiskResources, MakeWeak(this)),
        Bootstrap_->GetConfig()->ExecAgent->SlotManager->DiskResourcesUpdatePeriod))
    , LocationPath_(NFS::GetRealPath(Config_->Path))
{ }

TFuture<void> TSlotLocation::Initialize()
{
    Enabled_ = true;

    return BIND([=, this_ = MakeStrong(this)] {
        try {
            NFS::MakeDirRecursive(Config_->Path, 0755);

            WaitFor(HealthChecker_->RunCheck())
                .ThrowOnError();

            ValidateMinimumSpace();

            for (int slotIndex = 0; slotIndex < SlotCount_; ++slotIndex) {
                for (auto sandboxKind : TEnumTraits<ESandboxKind>::GetDomainValues()) {
                    auto sandboxPath = GetSandboxPath(slotIndex, sandboxKind);

                    try { 
                        if (!NFS::Exists(sandboxPath)) {
                            continue;
                        }

                        if (NFS::IsDirEmpty(sandboxPath)) {
                            continue;
                        }
                    } catch (const std::exception& ex) {
                        // In case of any errors (e.g. no permissions) we swallow exception and
                        // fallback to removing slots.
                    }

                    if (Bootstrap_->IsSimpleEnvironment()) {
                        NFS::RemoveRecursive(sandboxPath);
                    } else {
                        RunTool<TRemoveDirAsRootTool>(sandboxPath);
                    }
                }

                CreateSandboxDirectories(slotIndex);
            }
        } catch (const std::exception& ex) {
            auto error = TError("Failed to initialize slot location %v", Config_->Path)
                << ex;
            Disable(error);
            return;
        }

        HealthChecker_->SubscribeFailed(BIND(&TSlotLocation::Disable, MakeWeak(this))
            .Via(HeavyInvoker_));
        HealthChecker_->Start();

        DiskResourcesUpdateExecutor_->Start();
    })
    .AsyncVia(HeavyInvoker_)
    .Run();
}

TFuture<std::vector<TString>> TSlotLocation::PrepareSandboxDirectories(int slotIndex, TUserSandboxOptions options)
{
    auto userId = SlotIndexToUserId_(slotIndex);
    auto sandboxPath = GetSandboxPath(slotIndex, ESandboxKind::User);

    bool sandboxTmpfs = WaitFor(BIND([=, this_ = MakeStrong(this)] {
        for (const auto& tmpfsVolume : options.TmpfsVolumes) {
            // TODO(gritukan): Implement a function that joins absolute path with a relative path and returns
            // real path without filesystem access.
            auto tmpfsPath = NFS::GetRealPath(NFS::CombinePaths(sandboxPath, tmpfsVolume.Path));
            if (tmpfsPath == sandboxPath) {
                return true;
            }
        }

        return false;
    })
    .AsyncVia(LightInvoker_)
    .Run())
    .ValueOrThrow();

    bool shouldApplyQuota = ((options.InodeLimit || options.DiskSpaceLimit) && !sandboxTmpfs);

    const auto& invoker = sandboxTmpfs
        ? LightInvoker_
        : HeavyInvoker_;

    return BIND([=, this_ = MakeStrong(this)] {
        ValidateEnabled();

        YT_LOG_DEBUG("Preparing sandbox directiories (SlotIndex: %v, SandboxTmpfs: %v)",
            slotIndex,
            sandboxTmpfs);

        if (shouldApplyQuota) {
            try {
                auto properties = TJobDirectoryProperties {options.DiskSpaceLimit, options.InodeLimit, userId};
                WaitFor(JobDirectoryManager_->ApplyQuota(sandboxPath, properties))
                    .ThrowOnError();
                {
                    TWriterGuard guard(SlotsLock_);
                    SlotsWithQuota_.insert(slotIndex);
                }
            } catch (const std::exception& ex) {
                auto error = TError(EErrorCode::QuotaSettingFailed, "Failed to set FS quota for a job sandbox")
                    << TErrorAttribute("sandbox_path", sandboxPath)
                    << ex;
                Disable(error);
                THROW_ERROR error;
            }
        }

        // This tmp sandbox is a temporary workaround for nirvana. We apply the same quota as we do for usual sandbox.
        if (options.DiskSpaceLimit || options.InodeLimit) {
            auto tmpPath = GetSandboxPath(slotIndex, ESandboxKind::Tmp);
            try {
                auto properties = TJobDirectoryProperties{options.DiskSpaceLimit, options.InodeLimit, userId};
                WaitFor(JobDirectoryManager_->ApplyQuota(tmpPath, properties))
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                auto error = TError(EErrorCode::QuotaSettingFailed, "Failed to set FS quota for a job tmp directory")
                    << TErrorAttribute("tmp_path", tmpPath)
                    << ex;
                Disable(error);
                THROW_ERROR error;
            }
        }

        {
            TWriterGuard guard(SlotsLock_);
            YT_VERIFY(OccupiedSlotToDiskLimit_.emplace(slotIndex, options.DiskSpaceLimit).second);
        }

        std::vector<TString> result;

        for (const auto& tmpfsVolume : options.TmpfsVolumes) {
            // TODO(gritukan): GetRealPath here can be replaced with some light analogue that does not access filesystem.
            auto tmpfsPath = NFS::GetRealPath(NFS::CombinePaths(sandboxPath, tmpfsVolume.Path));
            try {
                if (tmpfsPath != sandboxPath) {
                    // If we mount directory inside sandbox, it should not exist.
                    ValidateNotExists(tmpfsPath);
                }
                NFS::MakeDirRecursive(tmpfsPath);
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Failed to create directory %v for tmpfs in sandbox %v",
                    tmpfsPath,
                    sandboxPath)
                    << ex;
            }

            if (!EnableTmpfs_) {
                continue;
            }

            try {
                auto properties = TJobDirectoryProperties{tmpfsVolume.Size, std::nullopt, userId};
                WaitFor(JobDirectoryManager_->CreateTmpfsDirectory(tmpfsPath, properties))
                    .ThrowOnError();

                {
                    TWriterGuard guard(SlotsLock_);
                    YT_VERIFY(TmpfsPaths_.insert(tmpfsPath).second);
                }

                result.push_back(tmpfsPath);
            } catch (const std::exception& ex) {
                // Job will be aborted.
                auto error = TError(EErrorCode::SlotLocationDisabled, "Failed to mount tmpfs %v into sandbox %v", tmpfsPath, sandboxPath)
                    << ex;
                Disable(error);
                THROW_ERROR error;
            }
        }

        for (int i = 0; i < result.size(); ++i) {
            for (int j = 0; j < result.size(); ++j) {
                if (i == j) {
                    continue;
                }
                auto lhsFsPath = TFsPath(result[i]);
                auto rhsFsPath = TFsPath(result[j]);
                if (lhsFsPath.IsSubpathOf(rhsFsPath)) {
                    THROW_ERROR_EXCEPTION("Path of tmpfs volume %v is prefix of other tmpfs volume %v",
                        result[i],
                        result[j]);
                }
            }
        }

        YT_LOG_DEBUG("Sandbox directories prepared (SlotIndex: %v)",
            slotIndex);

        return result;
    })
    .AsyncVia(invoker)
    .Run();
}

TFuture<void> TSlotLocation::DoMakeSandboxFile(
    int slotIndex,
    ESandboxKind kind,
    const std::function<void(const TString& destinationPath)>& callback,
    const TString& destinationName,
    bool canUseLightInvoker)
{
    auto sandboxPath = GetSandboxPath(slotIndex, kind);
    auto destinationPath = NFS::CombinePaths(sandboxPath, destinationName);

    bool useLightInvoker = (canUseLightInvoker && IsInsideTmpfs(destinationPath)); 
    const auto& invoker = useLightInvoker
        ? LightInvoker_
        : HeavyInvoker_;

    return BIND([=, this_ = MakeStrong(this)] {
        ValidateEnabled();

        YT_LOG_DEBUG("Making sandbox file (DestinationName: %v, UseLightInvoker: %v, SlotIndex: %v)",
            destinationName,
            useLightInvoker,
            slotIndex);

        try {
            // This validations do not disable slot.
            ValidateNotExists(destinationPath);
            ForceSubdirectories(destinationPath, sandboxPath);
        } catch (const std::exception& ex) {
            // Job will be failed.
            THROW_ERROR_EXCEPTION(
                "Failed to build file %Qv in sandbox %v",
                destinationName,
                sandboxPath)
                << ex;
        }

        auto processError = [&] (const std::exception& ex, bool noSpace) {
            bool slotWithQuota = false;
            {
                TReaderGuard guard(SlotsLock_);
                slotWithQuota = SlotsWithQuota_.contains(slotIndex);
            }

            if (IsInsideTmpfs(destinationPath) && noSpace) {
                THROW_ERROR_EXCEPTION(
                    EErrorCode::TmpfsOverflow,
                    "Failed to build file %Qv in sandbox %v: tmpfs is too small",
                    destinationName,
                    sandboxPath)
                    << ex;                
            } else if (slotWithQuota && noSpace) {
                THROW_ERROR_EXCEPTION(
                    "Failed to build file %Qv in sandbox %v: disk space limit is too small",
                    destinationName,
                    sandboxPath)
                    << ex;
            } else {
                // Probably location error, job will be aborted.
                auto error = TError(
                    EErrorCode::ArtifactCopyingFailed,
                    "Failed to build file %Qv in sandbox %v",
                    destinationName,
                    sandboxPath)
                    << ex;
                Disable(error);
                THROW_ERROR error;
            }
        };

        try {
            callback(destinationPath);
            EnsureNotInUse(destinationPath);
        } catch (const TErrorException& ex) {
            bool noSpace = static_cast<bool>(ex.Error().FindMatching(ELinuxErrorCode::NOSPC));
            processError(ex, noSpace);
        } catch (const TSystemError& ex) {
            // For util functions.
            bool noSpace = (ex.Status() == ENOSPC);
            processError(ex, noSpace);
        } catch (const std::exception& ex) {
            processError(ex, /* noSpace */false);
        }

        YT_LOG_DEBUG("Sandbox file created (DestinationName: %v, SlotIndex: %v)",
            destinationName,
            slotIndex);
    })
    .AsyncVia(invoker)
    .Run();
}

TFuture<void> TSlotLocation::MakeSandboxCopy(
    int slotIndex,
    ESandboxKind kind,
    const TString& sourcePath,
    const TString& destinationName,
    bool executable)
{
    return DoMakeSandboxFile(
        slotIndex,
        kind,
        [=] (const TString& destinationPath) {
            YT_LOG_DEBUG("Started copying file to sandbox (SourcePath: %v, DestinationName: %v)",
                sourcePath,
                destinationName);

            NFS::ChunkedCopy(
                sourcePath,
                destinationPath,
                Bootstrap_->GetConfig()->ExecAgent->SlotManager->FileCopyChunkSize);

            NFS::SetPermissions(destinationPath, 0666 + (executable ? 0111 : 0));

            YT_LOG_DEBUG("Finished copying file to sandbox (SourcePath: %v, DestinationName: %v)",
                sourcePath,
                destinationName);
        },
        destinationName,
        /* canUseLightInvoker */IsInsideTmpfs(sourcePath));
}

TFuture<void> TSlotLocation::MakeSandboxLink(
    int slotIndex,
    ESandboxKind kind,
    const TString& targetPath,
    const TString& linkName,
    bool executable)
{
    return DoMakeSandboxFile(
        slotIndex,
        kind,
        [=] (const TString& linkPath) {
            YT_LOG_DEBUG("Started making sandbox symlink (TargetPath: %v, LinkName: %v)",
                targetPath,
                linkName);

            // NB: Set permissions for the link _source_ and prevent writes to it.
            NFS::SetPermissions(targetPath, 0644 + (executable ? 0111 : 0));

            NFS::MakeSymbolicLink(targetPath, linkPath);

            YT_LOG_DEBUG("Finished making sandbox symlink (TargetPath: %v, LinkName: %v)",
                targetPath,
                linkName);
        },
        linkName,
        /* canUseLightInvoker */true);
}

TFuture<void> TSlotLocation::MakeSandboxFile(
    int slotIndex,
    ESandboxKind kind,
    const std::function<void(IOutputStream*)>& producer,
    const TString& destinationName,
    bool executable)
{
    return DoMakeSandboxFile(
        slotIndex,
        kind,
        [=] (const TString& destinationPath) {
            YT_LOG_DEBUG("Started building sandbox file (DestinationName: %v)",
                destinationName);

            TFile file(destinationPath, CreateAlways | WrOnly | Seq | CloseOnExec);
            file.Flock(LOCK_EX);

            TFileOutput stream(file);
            producer(&stream);

            NFS::SetPermissions(destinationPath, 0666 + (executable ? 0111 : 0));

            YT_LOG_DEBUG("Finished building sandbox file (DestinationName: %v)",
                destinationName);
        },
        destinationName,
        /* canUseLightInvoker */true);
}

TFuture<void> TSlotLocation::FinalizeSandboxPreparation(int slotIndex)
{
    auto sandboxPath = GetSandboxPath(slotIndex, ESandboxKind::User);
    const auto& invoker = IsInsideTmpfs(sandboxPath)
        ? LightInvoker_
        : HeavyInvoker_;

    return BIND([=, this_ = MakeStrong(this)] {
        YT_LOG_DEBUG("Finalizing sandbox preparation (SlotIndex: %v)",
            slotIndex);

        ValidateEnabled();

        auto userId = SlotIndexToUserId_(slotIndex);

        // We need to give read access to sandbox directory to yt_node/yt_job_proxy effective user (usually yt:yt)
        // and to job user (e.g. yt_slot_N). Since they can have different groups, we fallback to giving read
        // access to everyone.
        // job proxy requires read access e.g. for getting tmpfs size.
        // Write access is for job user only, who becomes an owner.
        try {
            ChownChmod(sandboxPath, userId, 0755);
        } catch (const std::exception& ex) {
            auto error = TError(EErrorCode::QuotaSettingFailed, "Failed to set owner and permissions for a job sandbox")
                << TErrorAttribute("sandbox_path", sandboxPath)
                << ex;
            Disable(error);
            THROW_ERROR error;
        }

        YT_LOG_DEBUG("Finalized sandbox preparation (SlotIndex: %v)",
            slotIndex);
    })
    .AsyncVia(invoker)
    .Run();
}

TFuture<void> TSlotLocation::MakeConfig(int slotIndex, INodePtr config)
{
    return BIND([=, this_ = MakeStrong(this)] {
        YT_LOG_DEBUG("Making job proxy config (SlotIndex: %v)",
            slotIndex);

        ValidateEnabled();
        auto proxyConfigPath = GetConfigPath(slotIndex);

        try {
            TFile file(proxyConfigPath, CreateAlways | WrOnly | Seq | CloseOnExec);
            TUnbufferedFileOutput output(file);
            TYsonWriter writer(&output, EYsonFormat::Pretty);
            Serialize(config, &writer);
            writer.Flush();
        } catch (const std::exception& ex) {
            // Job will be aborted.
            auto error = TError(EErrorCode::SlotLocationDisabled, "Failed to write job proxy config into %v",
                proxyConfigPath)
                << ex;
            Disable(error);
            THROW_ERROR error;
        }

        YT_LOG_DEBUG("Job proxy config written (SlotIndex: %v)",
            slotIndex);
    })
    // NB(gritukan): Job proxy config is written to the disk, but it should be fast
    // under reasonable circumstances, so we use light invoker here. 
    .AsyncVia(LightInvoker_)
    .Run();
}

TFuture<void> TSlotLocation::CleanSandboxes(int slotIndex)
{
    return BIND([=, this_ = MakeStrong(this)] {
        YT_LOG_DEBUG("Sandboxes cleaning started (SlotIndex: %v)",
            slotIndex);

        ValidateEnabled();

        {
            TWriterGuard guard(SlotsLock_);

            // There may be no slotIndex in this map
            // (e.g. during SlotMananager::Initialize)
            OccupiedSlotToDiskLimit_.erase(slotIndex);
        }

        try {
            for (auto sandboxKind : TEnumTraits<ESandboxKind>::GetDomainValues()) {
                const auto& sandboxPath = GetSandboxPath(slotIndex, sandboxKind);
                if (!NFS::Exists(sandboxPath)) {
                    continue;
                }

                YT_LOG_DEBUG("Removing job directories (Path: %v)", sandboxPath);

                WaitFor(JobDirectoryManager_->CleanDirectories(sandboxPath))
                    .ThrowOnError();

                YT_LOG_DEBUG("Cleaning sandbox directory (Path: %v)", sandboxPath);

                if (Bootstrap_->IsSimpleEnvironment()) {
                    NFS::RemoveRecursive(sandboxPath);
                } else {
                    RunTool<TRemoveDirAsRootTool>(sandboxPath);
                }

                {
                    TWriterGuard guard(SlotsLock_);

                    auto it = TmpfsPaths_.lower_bound(sandboxPath);
                    while (it != TmpfsPaths_.end() && it->StartsWith(sandboxPath)) {
                        it = TmpfsPaths_.erase(it);
                    }

                    SlotsWithQuota_.erase(slotIndex);
                }
            }

            // Prepare slot for the next job.
            CreateSandboxDirectories(slotIndex);
        } catch (const std::exception& ex) {
            auto error = TError("Failed to clean sandbox directories")
                << ex;
            Disable(error);
            THROW_ERROR error;
        }

        YT_LOG_DEBUG("Sandboxes cleaning finished (SlotIndex: %v)",
            slotIndex);
    })
    .AsyncVia(HeavyInvoker_)
    .Run();
}

void TSlotLocation::IncreaseSessionCount()
{
    ++SessionCount_;
}

void TSlotLocation::DecreaseSessionCount()
{
    --SessionCount_;
}

void TSlotLocation::ValidateNotExists(const TString& path)
{
    if (NFS::Exists(path)) {
        THROW_ERROR_EXCEPTION("Path %v already exists", path);
    }
}

void TSlotLocation::EnsureNotInUse(const TString& path) const
{
    // Take exclusive lock in blocking fashion to ensure that no
    // forked process is holding an open descriptor to the source file.
    TFile file(path, RdOnly | CloseOnExec);
    file.Flock(LOCK_EX);
}

TString TSlotLocation::GetConfigPath(int slotIndex) const
{
    return NFS::CombinePaths(GetSlotPath(slotIndex), ProxyConfigFileName);
}

TString TSlotLocation::GetSlotPath(int slotIndex) const
{
    return NFS::CombinePaths(LocationPath_, Format("%v", slotIndex));
}

TString TSlotLocation::GetMediumName() const
{
    return Config_->MediumName;
}

NChunkClient::TMediumDescriptor TSlotLocation::GetMediumDescriptor() const
{
    return MediumDescriptor_.Load();
}

void TSlotLocation::SetMediumDescriptor(const NChunkClient::TMediumDescriptor& descriptor)
{
    MediumDescriptor_.Store(descriptor);
}

TString TSlotLocation::GetSandboxPath(int slotIndex, ESandboxKind sandboxKind) const
{
    const auto& sandboxName = SandboxDirectoryNames[sandboxKind];
    YT_ASSERT(sandboxName);
    return NFS::CombinePaths(GetSlotPath(slotIndex), sandboxName);
}

bool TSlotLocation::IsInsideTmpfs(const TString& path) const
{
    TReaderGuard guard(SlotsLock_);

    auto it = TmpfsPaths_.lower_bound(path);
    if (it != TmpfsPaths_.begin()) {
        --it;
        if (path.StartsWith(*it + "/")) {
            return true;
        }
    }

    return false;
}

void TSlotLocation::ForceSubdirectories(const TString& filePath, const TString& sandboxPath) const
{
    auto dirPath = NFS::GetDirectoryName(filePath);
    if (!dirPath.StartsWith(sandboxPath)) {
        THROW_ERROR_EXCEPTION("Path of the file must be inside the sandbox directory")
            << TErrorAttribute("sandbox_path", sandboxPath)
            << TErrorAttribute("file_path", filePath);
    }
    NFS::MakeDirRecursive(dirPath);
}

void TSlotLocation::ValidateEnabled() const
{
    if (!IsEnabled()) {
        THROW_ERROR_EXCEPTION(
            EErrorCode::SlotLocationDisabled,
            "Slot location at %v is disabled",
            Config_->Path);
    }
}

void TSlotLocation::Disable(const TError& error)
{
    if (!Enabled_.exchange(false)) {
        return;
    }

    auto alert = TError(
        EErrorCode::SlotLocationDisabled,
        "Slot location at %v is disabled",
        Config_->Path)
        << error;

    YT_LOG_ERROR(alert);
    YT_VERIFY(!Logger.GetAbortOnAlert());

    auto masterConnector = Bootstrap_->GetMasterConnector();
    masterConnector->RegisterAlert(alert);

    DiskResourcesUpdateExecutor_->Stop();
}

void TSlotLocation::InvokeUpdateDiskResources()
{
    DiskResourcesUpdateExecutor_->ScheduleOutOfBand();
}

void TSlotLocation::UpdateDiskResources()
{
    if (!IsEnabled()) {
        return;
    }

    YT_LOG_DEBUG("Updating disk resources");

    try {
        auto locationStatistics = NFS::GetDiskSpaceStatistics(Config_->Path);
        i64 diskLimit = locationStatistics.TotalSpace;
        if (Config_->DiskQuota) {
            diskLimit = Min(diskLimit, *Config_->DiskQuota);
        }

        i64 diskUsage = 0;
        THashMap<int, std::optional<i64>> occupiedSlotToDiskLimit;

        {
            TReaderGuard guard(SlotsLock_);
            occupiedSlotToDiskLimit = OccupiedSlotToDiskLimit_;
        }

        for (const auto& pair : occupiedSlotToDiskLimit) {
            auto slotIndex = pair.first;
            const auto& slotDiskLimit = pair.second;
            if (!slotDiskLimit) {
                for (auto sandboxKind : TEnumTraits<ESandboxKind>::GetDomainValues()) {
                    auto path = GetSandboxPath(slotIndex, sandboxKind);
                    if (NFS::Exists(path)) {
                        // We have to calculate user directory size as root,
                        // because user job could have set restricted permissions for files and
                        // directories inside sandbox.
                        auto dirSize = (sandboxKind == ESandboxKind::User && !Bootstrap_->IsSimpleEnvironment())
                            ? RunTool<TGetDirectorySizeAsRootTool>(path)
                            : NFS::GetDirectorySize(path);
                        diskUsage += dirSize;
                    }
                }
            } else {
                diskUsage += *slotDiskLimit;
            }
        }

        auto availableSpace = Max<i64>(0, Min(locationStatistics.AvailableSpace, diskLimit - diskUsage));
        diskLimit = Min(diskLimit, diskUsage + availableSpace);

        diskLimit -= Config_->DiskUsageWatermark;

        YT_LOG_DEBUG("Disk info (Path: %v, Usage: %v, Limit: %v, Medium: %v)",
            Config_->Path,
            diskUsage,
            diskLimit,
            Config_->MediumName);


        auto mediumDescriptor = GetMediumDescriptor();
        if (mediumDescriptor.Index != NChunkClient::GenericMediumIndex) {
            auto guard = TWriterGuard(DiskResourcesLock_);
            DiskResources_.set_usage(diskUsage);
            DiskResources_.set_limit(diskLimit);
            DiskResources_.set_medium_index(mediumDescriptor.Index);
        }
    } catch (const std::exception& ex) {
        auto error = TError("Failed to get disk info") << ex;
        YT_LOG_WARNING(error);
        Disable(error);
    }

    YT_LOG_DEBUG("Disk resources updated");
}

NNodeTrackerClient::NProto::TDiskLocationResources TSlotLocation::GetDiskResources() const
{
    auto guard = TReaderGuard(DiskResourcesLock_);
    return DiskResources_;
}

void TSlotLocation::CreateSandboxDirectories(int slotIndex)
{
    auto userId = SlotIndexToUserId_(slotIndex);

    YT_LOG_DEBUG("Creating sandbox directories (SlotIndex: %v, UserId: %v)",
        slotIndex,
        userId);

    auto slotPath = GetSlotPath(slotIndex);
    try {
        NFS::MakeDirRecursive(slotPath, 0755);

        for (auto sandboxKind : TEnumTraits<ESandboxKind>::GetDomainValues()) {
            auto sandboxPath = GetSandboxPath(slotIndex, sandboxKind);
            NFS::MakeDirRecursive(sandboxPath, 0700);
        }

        // Since we make slot user to be owner, but job proxy creates some files during job shell
        // initialization we leave write access for everybody. Presumably this will not ruin job isolation.
        ChownChmod(GetSandboxPath(slotIndex, ESandboxKind::Home), userId, 0777);

        // Tmp is accessible for everyone.
        ChownChmod(GetSandboxPath(slotIndex, ESandboxKind::Tmp), userId, 0777);

        // CUDA library should have an access to cores directory to write GPU core dump into it.
        ChownChmod(GetSandboxPath(slotIndex, ESandboxKind::Cores), userId, 0777);

        // Pipes are accessible for everyone.
        ChownChmod(GetSandboxPath(slotIndex, ESandboxKind::Pipes), userId, 0777);
    } catch (const std::exception& ex) {
        auto error = TError(EErrorCode::SlotLocationDisabled, "Failed to create sandbox directories for slot %v", slotPath)
            << ex;
        Disable(error);
    }

    YT_LOG_DEBUG("Sandbox directories created (SlotIndex: %v)", slotIndex);
}

void TSlotLocation::ChownChmod(
    const TString& path,
    int userId,
    int permissions)
{
    if (Bootstrap_->IsSimpleEnvironment()) {
        ChownChmodDirectoriesRecursively(path, std::nullopt, permissions);
    } else {
        auto config = New<TChownChmodConfig>();

        config->Permissions = permissions;
        config->Path = path;
        config->UserId = static_cast<uid_t>(userId);
        RunTool<TChownChmodTool>(config);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecAgent
