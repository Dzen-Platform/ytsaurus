#pragma once

#include "public.h"
#include "serialize.h"

#include <yt/server/chunk_pools/public.h>

#include <yt/server/scheduler/job.h>

namespace NYT {
namespace NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

struct TBriefJobStatistics
    : public TIntrinsicRefCounted
{
    TInstant Timestamp = TInstant::Zero();

    i64 ProcessedInputRowCount = 0;
    i64 ProcessedInputUncompressedDataSize = 0;
    i64 ProcessedInputDataWeight = 0;
    i64 ProcessedInputCompressedDataSize = 0;
    i64 ProcessedOutputRowCount = 0;
    i64 ProcessedOutputUncompressedDataSize = 0;
    i64 ProcessedOutputCompressedDataSize = 0;
    // Time is given in milliseconds.
    TNullable<i64> InputPipeIdleTime = Null;
    TNullable<i64> JobProxyCpuUsage = Null;

    void Persist(const NPhoenix::TPersistenceContext& context);
};

DEFINE_REFCOUNTED_TYPE(TBriefJobStatistics)

void Serialize(const TBriefJobStatisticsPtr& briefJobStatistics, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

TBriefJobStatisticsPtr BuildBriefStatistics(std::unique_ptr<NScheduler::TJobSummary> jobSummary);

// Returns true if job proxy wasn't stalling and false otherwise.
// This function is related to the suspicious jobs detection.
bool CheckJobActivity(
    const TBriefJobStatisticsPtr& lhs,
    const TBriefJobStatisticsPtr& rhs,
    i64 cpuUsageThreshold,
    double inputPipeIdleTimeFraction);

// Performs statistics parsing and put it inside jobSummary.
void ParseStatistics(NScheduler::TJobSummary* jobSummary, const NYson::TYsonString& lastObservedStatisticsYson = NYson::TYsonString());

NYson::TYsonString BuildInputPaths(
    const std::vector<NYPath::TRichYPath>& inputPaths,
    const NChunkPools::TChunkStripeListPtr& inputStripeList,
    EOperationType operationType,
    EJobType jobType);

////////////////////////////////////////////////////////////////////////////////

struct TScheduleJobStatistics
    : public TIntrinsicRefCounted
    , public IPersistent
{
    void RecordJobResult(const NScheduler::TScheduleJobResultPtr& scheduleJobResult);

    TEnumIndexedVector<int, NScheduler::EScheduleJobFailReason> Failed;
    TDuration Duration;
    i64 Count = 0;

    void Persist(const TPersistenceContext& context);
};

DEFINE_REFCOUNTED_TYPE(TScheduleJobStatistics)

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT
