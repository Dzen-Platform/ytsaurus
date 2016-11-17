#include "slot_location.h"
#include "private.h"
#include "config.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/data_node/master_connector.h>

#include <yt/server/misc/disk_health_checker.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/fs.h>
#include <yt/core/misc/proc.h>

#include <yt/core/misc/singleton.h>

#include <yt/core/yson/writer.h>
#include <yt/core/ytree/convert.h>

#include <yt/core/tools/tools.h>

#include <util/system/fs.h>

namespace NYT {
namespace NExecAgent {

using namespace NConcurrency;
using namespace NTools;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TMounter
{
public:
    ~TMounter()
    { }

    static TMounter* Get()
    {
        return Singleton<TMounter>();
    }

    std::vector<NFS::TMountPoint> GetMountPoints()
    {
        auto invoker = Thread_->GetInvoker();
        auto asyncResult = BIND(NFS::GetMountPoints)
            .AsyncVia(invoker)
            .Run("/proc/mounts");
        auto result = WaitFor(asyncResult)
            .ValueOrThrow();
        return result;
    }

    void Mount(TMountTmpfsConfigPtr config)
    {
        RunTool<TMountTmpfsAsRootTool>(config);
    }

    void Umount(TUmountConfigPtr config)
    {
        RunTool<TUmountAsRootTool>(config);
    }


private:
    const TActionQueuePtr Thread_ = New<TActionQueue>("Mounter");
};

////////////////////////////////////////////////////////////////////////////////

TSlotLocation::TSlotLocation(
    const TSlotLocationConfigPtr& config,
    const NCellNode::TBootstrap* bootstrap,
    const Stroka& id,
    bool detachedTmpfsUmount)
    : TDiskLocation(config, id, ExecAgentLogger)
    , Config_(config)
    , Bootstrap_(bootstrap)
    , LocationQueue_(New<TActionQueue>(id))
    , DetachedTmpfsUmount_(detachedTmpfsUmount)
    , HasRootPermissions_(HasRootPermissions())
{
    Enabled_ = true;

    HealthChecker_ = New<TDiskHealthChecker>(
        bootstrap->GetConfig()->DataNode->DiskHealthChecker,
        Config_->Path,
        LocationQueue_->GetInvoker(),
        Logger);

    try {
        NFS::ForcePath(Config_->Path, 0755);
        HealthChecker_->RunCheck();

        ValidateMinimumSpace();
    } catch (const std::exception& ex) {
        auto error = TError("Failed to initialize slot location %v", Config_->Path) << ex;
        Disable(error);
        return;
    }

    HealthChecker_->SubscribeFailed(BIND(&TSlotLocation::Disable, MakeWeak(this))
        .Via(LocationQueue_->GetInvoker()));
    HealthChecker_->Start();
}

TFuture<void> TSlotLocation::CreateSandboxDirectories(int slotIndex)
{
    return BIND([=, this_ = MakeStrong(this)] () {
         ValidateEnabled();

         LOG_DEBUG("Making sandbox directiories (SlotIndex: %v)", slotIndex);

         auto slotPath = GetSlotPath(slotIndex);
         try {
             NFS::ForcePath(slotPath, 0755);

             for (auto sandboxKind : TEnumTraits<ESandboxKind>::GetDomainValues()) {
                 auto sandboxPath = GetSandboxPath(slotIndex, sandboxKind);
                 NFS::ForcePath(sandboxPath, 0777);
             }
         } catch (const std::exception& ex) {
             auto error = TError("Failed to create sandbox directories for slot %v", slotPath)
                << ex;
             Disable(error);
             THROW_ERROR error;
         }
    })
    .AsyncVia(LocationQueue_->GetInvoker())
    .Run();
}

TFuture<void> TSlotLocation::MakeSandboxCopy(
    int slotIndex,
    ESandboxKind kind,
    const Stroka& sourcePath,
    const Stroka& destinationName,
    bool executable)
{
    return BIND([=, this_ = MakeStrong(this)] () {
        ValidateEnabled();

        auto sandboxPath = GetSandboxPath(slotIndex, kind);
        auto destinationPath = NFS::CombinePaths(sandboxPath, destinationName);

        LOG_DEBUG("Making sandbox copy (SourcePath: %v, DestinationName: %v)",
            sourcePath,
            destinationName);

        try {
            // This validations do not disable slot.
            ValidateNotExists(destinationPath);
        } catch (const std::exception& ex) {
            // Job will be failed.
            THROW_ERROR_EXCEPTION(
                "Failed to make a copy for file %Qv into sandbox %v",
                destinationName,
                sandboxPath)
                    << ex;
        }

        auto logErrorAndDisableLocation = [&] (const std::exception& ex) {
            // Probably location error, job will be aborted.
            auto error = TError(
                EErrorCode::ArtifactCopyingFailed,
                "Failed to make a copy for file %Qv into sandbox %v",
                destinationName,
                sandboxPath) << ex;
            Disable(error);
            THROW_ERROR error;
        };

        try {
            NFS::ChunkedCopy(
                sourcePath,
                destinationPath,
                Bootstrap_->GetConfig()->ExecAgent->SlotManager->FileCopyChunkSize);
            EnsureNotInUse(destinationPath);
            NFS::SetExecutableMode(destinationPath, executable);
        } catch (const TErrorException& ex) {
            if (IsInsideTmpfs(destinationPath) && ex.Error().FindMatching(ELinuxErrorCode::NOSPC)) {
                THROW_ERROR_EXCEPTION("Failed to make a copy for file %Qv into sandbox %v: tmpfs is too small",
                    destinationName,
                    sandboxPath) << ex;
            } else {
                logErrorAndDisableLocation(ex);
            }
        } catch (const std::exception& ex) {
            logErrorAndDisableLocation(ex);
        }
    })
    .AsyncVia(LocationQueue_->GetInvoker())
    .Run();
}

TFuture<void> TSlotLocation::MakeSandboxLink(
    int slotIndex,
    ESandboxKind kind,
    const Stroka& targetPath,
    const Stroka& linkName,
    bool executable)
{
    return BIND([=, this_ = MakeStrong(this)] () {
        ValidateEnabled();

        auto sandboxPath = GetSandboxPath(slotIndex, kind);
        auto linkPath = NFS::CombinePaths(sandboxPath, linkName);

        LOG_DEBUG("Making sandbox symlink (TargetPath: %v, LinkName: %v)", targetPath, linkName);

        try {
            // This validations do not disable slot.
            ValidateNotExists(linkPath);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Failed to make a symlink %Qv into sandbox %v", linkName, sandboxPath)
                << ex;
        }

        try {
            EnsureNotInUse(targetPath);
            NFS::SetExecutableMode(targetPath, executable);
            NFS::MakeSymbolicLink(targetPath, linkPath);
        } catch (const std::exception& ex) {
            auto error = TError("Failed to make a symlink %Qv into sandbox %v", linkName, sandboxPath)
                << ex;
            Disable(error);
            THROW_ERROR error;
        }
    })
    .AsyncVia(LocationQueue_->GetInvoker())
    .Run();
}

TFuture<Stroka> TSlotLocation::MakeSandboxTmpfs(
    int slotIndex,
    ESandboxKind kind,
    i64 size,
    int userId,
    const Stroka& path,
    bool enable)
{
    return BIND([=, this_ = MakeStrong(this)] () {
        ValidateEnabled();
        auto sandboxPath = GetSandboxPath(slotIndex, kind);
        auto tmpfsPath = NFS::GetRealPath(NFS::CombinePaths(sandboxPath, path));
        auto isSandbox = tmpfsPath == sandboxPath;

        try {
            // This validations do not disable slot.
            if (!HasRootPermissions_) {
                THROW_ERROR_EXCEPTION("Sandbox tmpfs in disabled since node doesn't have root permissions");
            }

            if (!sandboxPath.is_prefix(tmpfsPath)) {
                THROW_ERROR_EXCEPTION("Path of the tmpfs mount point must be inside the sandbox directory")
                    << TErrorAttribute("sandbox_path", sandboxPath)
                    << TErrorAttribute("tmpfs_path", tmpfsPath);
            }

            if (!isSandbox) {
                // If we mount directory inside sandbox, it should not exist.
                ValidateNotExists(tmpfsPath);
            }

            NFS::ForcePath(tmpfsPath);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Failed to create directory %Qv for tmpfs in sandbox %v", path, sandboxPath)
                << ex;
        }

        if (!enable) {
            // Skip actual tmpfs mount.
            return tmpfsPath;
        }

        try {
            auto config = New<TMountTmpfsConfig>();
            config->Path = tmpfsPath;
            config->Size = size;

            // If we mount the whole sandbox, we use current process uid instead of slot one.
            config->UserId = isSandbox ? ::geteuid() : userId;

            LOG_DEBUG("Mounting tmpfs %v", ConvertToYsonString(config, EYsonFormat::Text));

            TMounter::Get()->Mount(config);
            if (isSandbox) {
                // We must give user full access to his sandbox.
                NFS::Chmod(tmpfsPath, 0777);
            }

            TmpfsPaths_.insert(tmpfsPath);
            return tmpfsPath;
        } catch (const std::exception& ex) {
            auto error = TError("Failed to mount tmpfs %v into sandbox %v", path, sandboxPath)
                << ex;
            Disable(error);
            THROW_ERROR error;
        }
    })
    .AsyncVia(LocationQueue_->GetInvoker())
    .Run();
}

TFuture<void> TSlotLocation::MakeConfig(int slotIndex, INodePtr config)
{
    return BIND([=, this_ = MakeStrong(this)] () {
        ValidateEnabled();
        auto proxyConfigPath = GetConfigPath(slotIndex);

        try {
            TFile file(proxyConfigPath, CreateAlways | WrOnly | Seq | CloseOnExec);
            TFileOutput output(file);
            TYsonWriter writer(&output, EYsonFormat::Pretty);
            Serialize(config, &writer);
            writer.Flush();
        } catch (const std::exception& ex) {
            auto error = TError("Failed to write job proxy config into %v", proxyConfigPath) << ex;
            Disable(error);
            THROW_ERROR error;
        }
    })
    .AsyncVia(LocationQueue_->GetInvoker())
    .Run();
}

TFuture<void> TSlotLocation::CleanSandboxes(int slotIndex)
{
    return BIND([=, this_ = MakeStrong(this)] () {
        ValidateEnabled();

        auto removeMountPoint = [this, this_ = MakeStrong(this)] (const Stroka& path) {
            auto config = New<TUmountConfig>();
            config->Path = path;
            config->Detach = DetachedTmpfsUmount_;

            try {
                // Due to bug in the kernel, this can sometimes fail with "Directory is not empty" error.
                // More info: https://bugzilla.redhat.com/show_bug.cgi?id=1066751
                RunTool<TRemoveDirContentAsRootTool>(path);
            } catch (const std::exception& ex) {
                LOG_WARNING(ex, "Failed to remove mount point %v", path);
            }

            TMounter::Get()->Umount(config);
        };

        for (auto sandboxKind : TEnumTraits<ESandboxKind>::GetDomainValues()) {
            const auto& sandboxPath = GetSandboxPath(slotIndex, sandboxKind);
            try {
                if (!NFS::Exists(sandboxPath)) {
                    continue;
                }

                LOG_DEBUG("Cleaning sandbox directory (Path: %v)", sandboxPath);

                auto sandboxFullPath = NFS::CombinePaths(~NFs::CurrentWorkingDirectory(), sandboxPath);
                auto isInsideSandbox = [&] (const Stroka& path) {
                    return path == sandboxFullPath || (sandboxFullPath + "/").is_prefix(path);
                };

                // Unmount all known tmpfs.
                std::vector<Stroka> tmpfsPaths;
                for (const auto& tmpfsPath : TmpfsPaths_) {
                    if (isInsideSandbox(tmpfsPath)) {
                        tmpfsPaths.push_back(tmpfsPath);
                    }
                }

                for (const auto& path : tmpfsPaths) {
                    LOG_DEBUG("Remove known mount point (Path: %v)", path);
                    TmpfsPaths_.erase(path);
                    removeMountPoint(path);
                }

                // Unmount unknown tmpfs, e.g. left from previous node run.

                // NB: iterating over /proc/mounts is not reliable,
                // see https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=593516.
                // To avoid problems with undeleting tmpfs ordered by user in sandbox
                // we always try to remove it several times.
                for (int attempt = 0; attempt < TmpfsRemoveAttemptCount; ++attempt) {
                    // Look mount points inside sandbox and unmount it.
                    auto mountPoints = TMounter::Get()->GetMountPoints();
                    for (const auto& mountPoint : mountPoints) {
                        if (isInsideSandbox(mountPoint.Path)) {
                            LOG_DEBUG("Remove unknown mount point (Path: %v)", mountPoint.Path);
                            removeMountPoint(mountPoint.Path);
                        }
                    }
                }

                if (HasRootPermissions_) {
                    RunTool<TRemoveDirAsRootTool>(sandboxPath);
                } else {
                    NFS::RemoveRecursive(sandboxPath);
                }
            } catch (const std::exception& ex) {
                auto error = TError("Failed to clean sandbox directory %v", sandboxPath) << ex;
                Disable(error);
                THROW_ERROR error;
            }
        }
    })
    .AsyncVia(LocationQueue_->GetInvoker())
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

void TSlotLocation::ValidateNotExists(const Stroka& path) const
{
    if (NFS::Exists(path)) {
        THROW_ERROR_EXCEPTION("Path %v already exists", path);
    }
}

void TSlotLocation::EnsureNotInUse(const Stroka& path) const
{
    // Take exclusive lock in blocking fashion to ensure that no
    // forked process is holding an open descriptor to the source file.
    TFile file(path, RdOnly | CloseOnExec);
    file.Flock(LOCK_EX);
}

Stroka TSlotLocation::GetConfigPath(int slotIndex) const
{
    return NFS::CombinePaths(GetSlotPath(slotIndex), ProxyConfigFileName);
}

Stroka TSlotLocation::GetSlotPath(int slotIndex) const
{
    return NFS::CombinePaths(Config_->Path, Format("%v", slotIndex));
}

Stroka TSlotLocation::GetSandboxPath(int slotIndex, ESandboxKind sandboxKind) const
{
    const auto& sandboxName = SandboxDirectoryNames[sandboxKind];
    Y_ASSERT(sandboxName);
    return NFS::CombinePaths(GetSlotPath(slotIndex), sandboxName);
}

bool TSlotLocation::IsInsideTmpfs(const Stroka& path) const
{
    for (const auto& tmpfsPath : TmpfsPaths_) {
        if (tmpfsPath.is_prefix(path)) {
            return true;
        }
    }

    return false;
}

void TSlotLocation::Disable(const TError& error)
{
    if (!Enabled_.exchange(false))
        return;

    auto alert = TError("Slot location at %v is disabled", Config_->Path) << error;

    LOG_ERROR(alert);

    auto masterConnector = Bootstrap_->GetMasterConnector();
    masterConnector->RegisterAlert(alert);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
