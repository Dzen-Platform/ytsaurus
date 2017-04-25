#pragma once

#include "public.h"
#include "private.h"

#include <yt/server/cell_scheduler/public.h>

#include <yt/server/misc/fork_executor.h>

#include <yt/ytlib/api/public.h>

#include <yt/core/pipes/pipe.h>

#include <yt/core/profiling/profiler.h>

#include <util/system/file.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct TSnapshotJob
    : public TIntrinsicRefCounted
{
    TOperationPtr Operation;
    NPipes::TAsyncReaderPtr Reader;
    std::unique_ptr<TFile> OutputFile;
    bool Suspended = false;
};

DEFINE_REFCOUNTED_TYPE(TSnapshotJob)

////////////////////////////////////////////////////////////////////////////////

class TSnapshotBuilder
    : public TForkExecutor
{
public:
    TSnapshotBuilder(
        TSchedulerConfigPtr config,
        TSchedulerPtr scheduler,
        NApi::IClientPtr client);

    TFuture<void> Run();

private:
    const TSchedulerConfigPtr Config_;
    const TSchedulerPtr Scheduler_;
    const NApi::IClientPtr Client_;

    std::vector<TSnapshotJobPtr> Jobs_;

    NProfiling::TProfiler Profiler;

    virtual TDuration GetTimeout() const override;
    virtual void RunParent() override;
    virtual void RunChild() override;

    TFuture<std::vector<TError>> UploadSnapshots();
    void UploadSnapshot(const TSnapshotJobPtr& job);

    bool ControllersSuspended_ = false;
};

DEFINE_REFCOUNTED_TYPE(TSnapshotBuilder)

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
