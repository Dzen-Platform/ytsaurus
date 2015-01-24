#pragma once

#include <core/misc/property.h>

#include <core/actions/callback.h>

#include <core/yson/consumer.h>

#include <core/ytree/tree_builder.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

class TSummary
{
public:
    TSummary();
    explicit TSummary(i64 value);

    void Merge(const TSummary& other);

    DEFINE_BYVAL_RO_PROPERTY(i64, Sum);
    DEFINE_BYVAL_RO_PROPERTY(i64, Count);
    DEFINE_BYVAL_RO_PROPERTY(i64, Min);
    DEFINE_BYVAL_RO_PROPERTY(i64, Max);

    friend void Deserialize(TSummary& value, NYTree::INodePtr node);
};

void Serialize(const TSummary& summary, NYson::IYsonConsumer* consumer);
void Deserialize(TSummary& value, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////

class TStatistics
{
public:
    void Add(const NYPath::TYPath& name, const TSummary& summary);

    template <class T>
    void Add(const NYPath::TYPath& path, const T& statistics);

    void Merge(const TStatistics& other);
    void Clear();
    bool IsEmpty() const;

    TSummary Get(const NYPath::TYPath& name) const;

private:
    yhash_map<NYPath::TYPath, TSummary> PathToSummary_;

    friend void Serialize(const TStatistics& statistics, NYson::IYsonConsumer* consumer);
    friend void Deserialize(TStatistics& value, NYTree::INodePtr node);
};

void Serialize(const TStatistics& statistics, NYson::IYsonConsumer* consumer);
void Deserialize(TStatistics& value, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////

class TStatisticsConsumer
    : public NYson::TYsonConsumerBase
{
public:
    typedef TCallback<void(const TStatistics&)> TParsedStatisticsConsumer;
    explicit TStatisticsConsumer(TParsedStatisticsConsumer consumer, const NYPath::TYPath& path);

    virtual void OnStringScalar(const TStringBuf& value) override;
    virtual void OnInt64Scalar(i64 value) override;
    virtual void OnUint64Scalar(ui64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnBooleanScalar(bool value) override;
    virtual void OnEntity() override;

    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnEndList() override;

    virtual void OnBeginMap() override;
    virtual void OnKeyedItem(const TStringBuf& key) override;
    virtual void OnEndMap() override;

    virtual void OnBeginAttributes() override;
    virtual void OnEndAttributes() override;

private:
    int Depth_ = 0;
    NYPath::TYPath Path_;
    std::unique_ptr<NYTree::ITreeBuilder> TreeBuilder_;
    TParsedStatisticsConsumer Consumer_;

    void ConvertToStatistics(TStatistics& value, NYTree::INodePtr node);
};

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

#define STATISTICS_INL_H_
#include "statistics-inl.h"
#undef STATISTICS_INL_H_
