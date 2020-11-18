#pragma once

#include "private.h"

#include "sorted_job_builder.h"

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/chunk_client/public.h>

namespace NYT::NChunkPools {

////////////////////////////////////////////////////////////////////////////////

//! An interface for the class that encapsulates the whole logic of building sorted* jobs.
//! This class defines a transient object (it is never persisted).
struct ILegacySortedJobBuilder
    : public TRefCounted
{
    virtual void AddForeignDataSlice(
        const NChunkClient::TLegacyDataSlicePtr& dataSlice,
        IChunkPoolInput::TCookie cookie) = 0;
    virtual void AddPrimaryDataSlice(
        const NChunkClient::TLegacyDataSlicePtr& dataSlice,
        IChunkPoolInput::TCookie cookie) = 0;
    virtual std::vector<std::unique_ptr<TJobStub>> Build() = 0;
    virtual i64 GetTotalDataSliceCount() const = 0;
};

DEFINE_REFCOUNTED_TYPE(ILegacySortedJobBuilder);

////////////////////////////////////////////////////////////////////////////////

ILegacySortedJobBuilderPtr CreateLegacySortedJobBuilder(
    const TSortedJobOptions& options,
    NControllerAgent::IJobSizeConstraintsPtr jobSizeConstraints,
    const NTableClient::TRowBufferPtr& rowBuffer,
    const std::vector<NChunkClient::TInputChunkPtr>& teleportChunks,
    bool inSplit,
    int reftryIndex,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools
