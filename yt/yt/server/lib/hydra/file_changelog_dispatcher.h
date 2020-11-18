#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/public.h>
#include <yt/ytlib/hydra/proto/hydra_manager.pb.h>

#include <yt/core/actions/public.h>

#include <yt/core/misc/shutdownable.h>

#include <yt/core/profiling/public.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

//! Provides a factory for creating new and opening existing file changelogs.
//! Manages a background thread that keeps track of unflushed changelogs and
//! issues flush requests periodically.
class TFileChangelogDispatcher
    : public TRefCounted
{
public:
    TFileChangelogDispatcher(
        const NChunkClient::IIOEnginePtr& ioEngine,
        const TFileChangelogDispatcherConfigPtr& config,
        const TString& threadName,
        const NProfiling::TRegistry& profiler);

    ~TFileChangelogDispatcher();

    //! Returns the invoker managed by the dispatcher.
    IInvokerPtr GetInvoker();

    //! Synchronously creates a new changelog.
    IChangelogPtr CreateChangelog(
        const TString& path,
        const TFileChangelogConfigPtr& config);

    //! Synchronously opens an existing changelog.
    IChangelogPtr OpenChangelog(
        const TString& path,
        const TFileChangelogConfigPtr& config);

    //! Flushes all active changelogs owned by this dispatcher.
    TFuture<void> FlushChangelogs();

private:
    class TImpl;
    using TImplPtr = TIntrusivePtr<TImpl>;

    friend class TFileChangelog;
    friend class TFileChangelogQueue;

    const TImplPtr Impl_;
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogDispatcher)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
