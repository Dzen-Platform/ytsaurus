#pragma once

#include <yt/core/misc/error.h>

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/ytlib/node_tracker_client/node_tracker_service.pb.h>

#include <yt/core/yson/string.h>

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_FORWARD_RW_PROPERTY(type, name) \
    DEFINE_BYREF_RO_PROPERTY(type, name) \
public: \
    template <class T> \
    Y_FORCE_INLINE auto&& name(T&& t) && \
    { \
        Set##name(std::forward<T>(t)); \
        return std::move(*this); \
    }

////////////////////////////////////////////////////////////////////////////////

namespace NYT {
namespace NJobAgent {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EReportPriority,
    (Normal)
    (Low)
);

////////////////////////////////////////////////////////////////////////////////

struct TJobEvent
{
    TJobEvent(NJobTrackerClient::EJobState state)
        : Timestamp_(Now())
        , State_(state)
    { }

    TJobEvent(NJobTrackerClient::EJobPhase phase)
        : Timestamp_(Now())
        , Phase_(phase)
    { }

    TJobEvent(NJobTrackerClient::EJobState state, NJobTrackerClient::EJobPhase phase)
        : Timestamp_(Now())
        , State_(state)
        , Phase_(phase)
    { }

    DEFINE_BYREF_RO_PROPERTY(TInstant, Timestamp)
    DEFINE_BYREF_RO_PROPERTY(TNullable<NJobTrackerClient::EJobState>, State)
    DEFINE_BYREF_RO_PROPERTY(TNullable<NJobTrackerClient::EJobPhase>, Phase)
};

using TJobEvents = std::vector<TJobEvent>;

////////////////////////////////////////////////////////////////////////////////

struct TJobStatistics
{
public:
    TJobStatistics();

    void SetPriority(EReportPriority priority);
    void SetOperationId(NJobTrackerClient::TOperationId operationId);
    void SetJobId(NJobTrackerClient::TJobId jobId);
    void SetType(NJobTrackerClient::EJobType type);
    void SetState(NJobTrackerClient::EJobState state);
    void SetStartTime(TInstant startTime);
    void SetFinishTime(TInstant finishTime);
    void SetError(const TError& error);
    void SetSpec(const NJobTrackerClient::NProto::TJobSpec& spec);
    void SetSpecVersion(i64 specVersion);
    void SetStatistics(const NYson::TYsonString& statistics);
    void SetEvents(const TJobEvents& events);

    DEFINE_FORWARD_RW_PROPERTY(EReportPriority, Priority)
    DEFINE_FORWARD_RW_PROPERTY(NJobTrackerClient::TOperationId, OperationId)
    DEFINE_FORWARD_RW_PROPERTY(NJobTrackerClient::TJobId, JobId)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<Stroka>, Type)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<Stroka>, State)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<i64>, StartTime)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<i64>, FinishTime)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<Stroka>, Error)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<Stroka>, Spec)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<i64>, SpecVersion)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<Stroka>, Statistics)
    DEFINE_FORWARD_RW_PROPERTY(TNullable<Stroka>, Events)
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobAgent
} // namespace NYT
