#pragma once

#include "public.h"

#include <yt/yt/core/yson/forwarding_consumer.h>
#include <yt/yt/core/yson/consumer.h>
#include <yt/yt/core/yson/building_consumer.h>

#include <yt/yt/core/ytree/tree_builder.h>
#include <yt/yt/core/ytree/convert.h>

#include <yt/yt/core/actions/callback.h>

#include <yt/yt/core/misc/property.h>

#include <util/generic/iterator_range.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TSummary
{
public:
    TSummary();

    TSummary(i64 sum, i64 count, i64 min, i64 max, std::optional<i64> last);

    void AddSample(i64 sample);

    void Merge(const TSummary& summary);

    void Reset();

    DEFINE_BYVAL_RO_PROPERTY(i64, Sum);
    DEFINE_BYVAL_RO_PROPERTY(i64, Count);
    DEFINE_BYVAL_RO_PROPERTY(i64, Min);
    DEFINE_BYVAL_RO_PROPERTY(i64, Max);
    DEFINE_BYVAL_RO_PROPERTY(std::optional<i64>, Last);

    void Persist(const TStreamPersistenceContext& context);

    bool operator == (const TSummary& other) const;

    friend class TStatisticsBuildingConsumer;
};

void Serialize(const TSummary& summary, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

class TStatistics
{
public:
    using TSummaryMap = std::map<NYPath::TYPath, TSummary>;
    using TSummaryRange = TIteratorRange<TSummaryMap::const_iterator>;
    DEFINE_BYREF_RO_PROPERTY(TSummaryMap, Data);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<TInstant>, Timestamp);

public:
    void AddSample(const NYPath::TYPath& path, i64 sample);

    void AddSample(const NYPath::TYPath& path, const NYTree::INodePtr& sample);

    template <class T>
    void AddSample(const NYPath::TYPath& path, const T& sample);

    void Merge(const TStatistics& statistics);

    void AddSuffixToNames(const TString& suffix);

    //! Get range of all elements whose path starts with a given strict prefix path (possibly empty).
    /*!
     * Pre-requisites: `prefixPath` must not have terminating slash.
     * Examples: /a/b is a prefix path for /a/bcd/efg but not for /a/b/hij nor /a/b itself.
     */
    TSummaryRange GetRangeByPrefix(const TString& prefixPath) const;

    void Persist(const TStreamPersistenceContext& context);

private:
    TSummary& GetSummary(const NYPath::TYPath& path);

    friend class TStatisticsBuildingConsumer;
};

i64 GetNumericValue(const TStatistics& statistics, const TString& path);

std::optional<i64> FindNumericValue(const TStatistics& statistics, const TString& path);
std::optional<TSummary> FindSummary(const TStatistics& statistics, const TString& path);

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TStatistics& statistics, NYson::IYsonConsumer* consumer);

void CreateBuildingYsonConsumer(std::unique_ptr<NYson::IBuildingYsonConsumer<TStatistics>>* buildingConsumer, NYson::EYsonType ysonType);

////////////////////////////////////////////////////////////////////////////////

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
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define STATISTICS_INL_H_
#include "statistics-inl.h"
#undef STATISTICS_INL_H_
