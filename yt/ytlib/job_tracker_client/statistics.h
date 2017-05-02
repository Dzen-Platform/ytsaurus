#pragma once

#include <yt/ytlib/chunk_client/data_statistics.pb.h>

#include <yt/ytlib/job_tracker_client/job.pb.h>

#include <yt/core/yson/forwarding_consumer.h>
#include <yt/core/yson/consumer.h>
#include <yt/core/yson/building_consumer.h>

#include <yt/core/ytree/tree_builder.h>
#include <yt/core/ytree/convert.h>

#include <yt/core/actions/callback.h>

#include <yt/core/misc/phoenix.h>
#include <yt/core/misc/property.h>

namespace NYT {
namespace NJobTrackerClient {

////////////////////////////////////////////////////////////////////

class TSummary
{
public:
    TSummary();

    TSummary(i64 sum, i64 count, i64 min, i64 max);

    void AddSample(i64 sample);

    void Update(const TSummary& summary);

    void Reset();

    DEFINE_BYVAL_RO_PROPERTY(i64, Sum);
    DEFINE_BYVAL_RO_PROPERTY(i64, Count);
    DEFINE_BYVAL_RO_PROPERTY(i64, Min);
    DEFINE_BYVAL_RO_PROPERTY(i64, Max);

    void Persist(NPhoenix::TPersistenceContext& context);

    bool operator == (const TSummary& other) const;

    friend class TStatisticsBuildingConsumer;
};

void Serialize(const TSummary& summary, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////

class TStatistics
{
public:
    using TSummaryMap = std::map<NYPath::TYPath, TSummary>;
    DEFINE_BYREF_RO_PROPERTY(TSummaryMap, Data);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, Timestamp);

public:
    void AddSample(const NYPath::TYPath& path, i64 sample);

    void AddSample(const NYPath::TYPath& path, const NYTree::INodePtr& sample);

    template <class T>
    void AddSample(const NYPath::TYPath& path, const T& sample);

    void Update(const TStatistics& statistics);

    void AddSuffixToNames(const Stroka& suffix);

    void Persist(NPhoenix::TPersistenceContext& context);

private:
    TSummary& GetSummary(const NYPath::TYPath& path);

    friend class TStatisticsBuildingConsumer;
};

i64 GetNumericValue(const TStatistics& statistics, const Stroka& path);

TNullable<i64> FindNumericValue(const TStatistics& statistics, const Stroka& path);

////////////////////////////////////////////////////////////////////

void Serialize(const TStatistics& statistics, NYson::IYsonConsumer* consumer);

void CreateBuildingYsonConsumer(std::unique_ptr<NYson::IBuildingYsonConsumer<TStatistics>>* buildingConsumer, NYson::EYsonType ysonType);

////////////////////////////////////////////////////////////////////

NChunkClient::NProto::TDataStatistics GetTotalInputDataStatistics(const TStatistics& jobStatistics);
NChunkClient::NProto::TDataStatistics GetTotalOutputDataStatistics(const TStatistics& jobStatistics);
yhash<int, NChunkClient::NProto::TDataStatistics> GetOutputDataStatistics(const TStatistics& jobStatistics);

////////////////////////////////////////////////////////////////////

class TStatisticsConsumer
    : public NYson::TForwardingYsonConsumer
{
public:
    typedef TCallback<void(const NYTree::INodePtr& sample)> TSampleHandler;
    explicit TStatisticsConsumer(TSampleHandler consumer);

private:
    const std::unique_ptr<NYTree::ITreeBuilder> TreeBuilder_;
    const TSampleHandler SampleHandler_;

    virtual void OnMyListItem() override;

    void ProcessSample();
};

////////////////////////////////////////////////////////////////////

} // namespace NJobTrackerClient
} // namespace NYT

#define STATISTICS_INL_H_
#include "statistics-inl.h"
#undef STATISTICS_INL_H_
