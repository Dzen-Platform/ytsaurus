#pragma once

#include "chunk_pool.h"
#include "private.h"
#include "job_manager.h"

#include <yt/ytlib/table_client/public.h>

namespace NYT::NLegacyChunkPools {

////////////////////////////////////////////////////////////////////////////////

struct TSortedJobOptions
{
    bool EnableKeyGuarantee = false;
    int PrimaryPrefixLength = 0;
    int ForeignPrefixLength = 0;
    bool EnablePeriodicYielder = true;
    bool ShouldSlicePrimaryTableByKeys = false;

    std::vector<NTableClient::TKey> PivotKeys;

    //! An upper bound for a total number of slices that is allowed. If this value
    //! is exceeded, an exception is thrown.
    i64 MaxTotalSliceCount;

    bool LogDetails = false;

    void Persist(const TPersistenceContext& context);
};

////////////////////////////////////////////////////////////////////////////////

//! An interface for the class that incapsulates the whole logic of building sorted* jobs.
//! This class defines a transient object (it is never persisted).
struct ISortedJobBuilder
    : public TRefCounted
{
    virtual void AddForeignDataSlice(
        const NChunkClient::TInputDataSlicePtr& dataSlice,
        IChunkPoolInput::TCookie cookie) = 0;
    virtual void AddPrimaryDataSlice(
        const NChunkClient::TInputDataSlicePtr& dataSlice,
        IChunkPoolInput::TCookie cookie) = 0;
    virtual std::vector<std::unique_ptr<TJobStub>> Build() = 0;
    virtual i64 GetTotalDataSliceCount() const = 0;
};

DEFINE_REFCOUNTED_TYPE(ISortedJobBuilder);

////////////////////////////////////////////////////////////////////////////////

ISortedJobBuilderPtr CreateSortedJobBuilder(
    const TSortedJobOptions& options,
    NControllerAgent::IJobSizeConstraintsPtr jobSizeConstraints,
    const NTableClient::TRowBufferPtr& rowBuffer,
    const std::vector<NChunkClient::TInputChunkPtr>& teleportChunks,
    bool inSplit,
    int reftryIndex,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLegacyChunkPools
