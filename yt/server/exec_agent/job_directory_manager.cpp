#include "job_directory_manager.h"

#include "private.h"

#include <yt/server/data_node/config.h>

#ifdef _linux_
#include <yt/server/containers/porto_executor.h>
#endif

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/misc/fs.h>
#include <yt/core/misc/proc.h>

#include <yt/core/tools/tools.h>

namespace NYT {
namespace NExecAgent {

using namespace NConcurrency;
#ifdef _linux_
using namespace NContainers;
#endif
using namespace NDataNode;
using namespace NTools;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

#ifdef _linux_

class TPortoJobDirectoryManager
    : public IJobDirectoryManager
{
public:
    explicit TPortoJobDirectoryManager(const TVolumeManagerConfigPtr& config)
        : Executor_(CreatePortoExecutor(config->PortoRetryTimeout, config->PortoPollPeriod))
    {
        // Since all the volumes should have been linked to the old node container,
        // there should be no volumes we could possibly restore.
    }

    virtual TFuture<void> ApplyQuota(const TString& path, const TJobDirectoryProperties& properties) override
    {
        return DoCreateVolume(path, properties, false);
    }

    virtual TFuture<void> CreateTmpfsDirectory(const TString& path, const TJobDirectoryProperties& properties) override
    {
        return DoCreateVolume(path, properties, true);
    }

    virtual TFuture<void> CleanDirectories(const TString& pathPrefix) override
    {
        std::vector<TString> toRelease;
        {
            auto guard = Guard(SpinLock_);
            auto it = ManagedVolumes_.lower_bound(pathPrefix);
            while (it != ManagedVolumes_.end() && (*it == pathPrefix || it->StartsWith(pathPrefix + "/"))) {
                toRelease.push_back(*it);
                it = ManagedVolumes_.erase(it);
            }
        }

        std::vector<TFuture<void>> asyncUnlinkResults;
        for (const auto& path : toRelease) {
            asyncUnlinkResults.emplace_back(Executor_->UnlinkVolume(path, "self"));
        }

        return Combine(asyncUnlinkResults);
    }

private:
    const IPortoExecutorPtr Executor_;

    TSpinLock SpinLock_;
    std::set<TString> ManagedVolumes_;

    TFuture<void> DoCreateVolume(const TString& path, const TJobDirectoryProperties& properties, bool isTmpfs)
    {
        std::map<TString, TString> volumeProperties;

        if (isTmpfs) {
            volumeProperties["backend"] = "tmpfs";
        } else if (properties.DiskSpaceLimit || properties.InodeLimit) {
            volumeProperties["backend"] = "quota";
        } else {
            return VoidFuture;
        }

        volumeProperties["user"] = ToString(properties.UserId);

        if (properties.DiskSpaceLimit) {
            volumeProperties["space_limit"] = ToString(*properties.DiskSpaceLimit);
        }

        if (properties.InodeLimit) {
            volumeProperties["inode_limit"] = ToString(*properties.InodeLimit);
        }

        auto onVolumeCreated = BIND([this_ = MakeStrong(this)] (const TVolumeId& volumeId) {
            auto guard = Guard(this_->SpinLock_);
            YCHECK(this_->ManagedVolumes_.insert(volumeId.Path).second);
        });

        return Executor_->CreateVolume(path, volumeProperties)
            .Apply(onVolumeCreated);
    }
};

IJobDirectoryManagerPtr CreatePortoJobDirectoryManager(TVolumeManagerConfigPtr config)
{
    return New<TPortoJobDirectoryManager>(std::move(config));
}

#endif

////////////////////////////////////////////////////////////////////////////////

class TSimpleJobDirectoryManager
    : public IJobDirectoryManager
{
public:
    TSimpleJobDirectoryManager(
        IInvokerPtr invoker,
        const TString& path,
        bool detachedTmpfsUmount)
        : Invoker_(std::move(invoker))
        , Path_(path)
        , DetachedTmpfsUmount_(detachedTmpfsUmount)
    {
        // NB: iterating over /proc/mounts is not reliable,
        // see https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=593516.
        // To avoid problems with undeleting tmpfs ordered by user in sandbox
        // we always try to remove it several times.
        for (int attempt = 0; attempt < TmpfsRemoveAttemptCount; ++attempt) {
            auto mountPoints = NFS::GetMountPoints("/proc/mounts");
            for (const auto& mountPoint : mountPoints) {
                if (mountPoint.Path == Path_ || mountPoint.Path.StartsWith(Path_ + "/")) {
                    Directories_.insert(mountPoint.Path);
                }
            }

            auto error = WaitFor(CleanDirectories(Path_));
            if (!error.IsOK()) {
                THROW_ERROR_EXCEPTION("Failed to initialize simple job directory manager")
                    << TErrorAttribute("path", Path_)
                    << error;
            }
        }
    }

    virtual TFuture<void> ApplyQuota(const TString& path, const TJobDirectoryProperties& properties) override
    {
        if (!properties.InodeLimit && !properties.DiskSpaceLimit) {
            return VoidFuture;
        }

        auto config = New<TFSQuotaConfig>();
        config->DiskSpaceLimit = properties.DiskSpaceLimit;
        config->InodeLimit = properties.InodeLimit;
        config->UserId = properties.UserId;
        config->Path = path;

        return BIND([=, this_ = MakeStrong(this)] () {
            return RunTool<TFSQuotaTool>(config);
        })
        .AsyncVia(Invoker_)
        .Run();
    }

    virtual TFuture<void> CreateTmpfsDirectory(const TString& path, const TJobDirectoryProperties& properties) override
    {
        auto config = New<TMountTmpfsConfig>();
        config->Path = path;
        config->Size = properties.DiskSpaceLimit.Get(std::numeric_limits<i64>::max());
        config->UserId = properties.UserId;

        LOG_DEBUG("Mounting tmpfs (Config: %v)",
            ConvertToYsonString(config, EYsonFormat::Text));

        return BIND([=, this_ = MakeStrong(this)] () {
            RunTool<TMountTmpfsAsRootTool>(config);
            YCHECK(Directories_.insert(path).second);
        })
        .AsyncVia(Invoker_)
        .Run();
    }

    virtual TFuture<void> CleanDirectories(const TString& pathPrefix) override
    {
        return BIND([=, this_ = MakeStrong(this)] () {
            std::vector<TString> toRelease;
            auto it = Directories_.lower_bound(pathPrefix);
            while (it != Directories_.end() && (*it == pathPrefix || it->StartsWith(pathPrefix + "/"))) {
                toRelease.push_back(*it);
                it = Directories_.erase(it);
            }

            for (const auto& path : toRelease) {
                try {
                    // Due to bug in the kernel, this can sometimes fail with "Directory is not empty" error.
                    // More info: https://bugzilla.redhat.com/show_bug.cgi?id=1066751
                    RunTool<TRemoveDirContentAsRootTool>(path);
                } catch (const std::exception& ex) {
                    LOG_WARNING(ex, "Failed to remove mount point (Path: %v)", path);
                }

                auto config = New<TUmountConfig>();
                config->Path = path;
                config->Detach = DetachedTmpfsUmount_;
                RunTool<TUmountAsRootTool>(config);
            }
        })
        .AsyncVia(Invoker_)
        .Run();
    }

private:
    const IInvokerPtr Invoker_;
    const TString Path_;
    const bool DetachedTmpfsUmount_;

    std::set<TString> Directories_;
};

IJobDirectoryManagerPtr CreateSimpleJobDirectoryManager(
    IInvokerPtr invoker,
    const TString& path,
    bool detachedTmpfsUmount)
{
    return New<TSimpleJobDirectoryManager>(
        std::move(invoker),
        path,
        detachedTmpfsUmount);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT