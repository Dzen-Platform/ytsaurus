#pragma once

#include "public.h"
#include "serialize.h"

#include <yt/server/controller_agent/chunk_pools/public.h>

#include <yt/core/ytree/fluent.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

struct IJobSplitter
    : public IPersistent
    , public NPhoenix::TFactoryTag<NPhoenix::TSimpleFactory>
{
    virtual void OnJobStarted(
        TJobId jobId,
        const NChunkPools::TChunkStripeListPtr& inputStripeList) = 0;
    virtual void OnJobRunning(const TJobSummary& summary) = 0;
    virtual void OnJobFailed(const TFailedJobSummary& summary) = 0;
    virtual void OnJobAborted(const TAbortedJobSummary& summary) = 0;
    virtual void OnJobCompleted(const TCompletedJobSummary& summary) = 0;
    virtual int EstimateJobCount(
        const TCompletedJobSummary& summary,
        i64 unreadRowCount) const = 0;
    virtual bool IsJobSplittable(TJobId jobId) const = 0;
    virtual void BuildJobSplitterInfo(NYTree::TFluentMap fluent) const = 0;
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IJobSplitter> CreateJobSplitter(
    const TJobSplitterConfigPtr& config,
    TOperationId operationId);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
