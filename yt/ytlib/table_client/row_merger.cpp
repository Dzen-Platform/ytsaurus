#include "stdafx.h"
#include "row_merger.h"
#include "config.h"
#include "row_buffer.h"

#include <ytlib/query_client/column_evaluator.h>

#include <ytlib/transaction_client/helpers.h>

namespace NYT {
namespace NTableClient {

using namespace NTransactionClient;
using namespace NQueryClient;

////////////////////////////////////////////////////////////////////////////////

TSchemafulRowMerger::TSchemafulRowMerger(
    TRowBufferPtr rowBuffer,
    int keyColumnCount,
    const TColumnFilter& columnFilter,
    TColumnEvaluatorPtr columnEvauator)
    : RowBuffer_(rowBuffer)
    , TableSchema_(columnEvauator->TableSchema())
    , KeyColumnCount_(keyColumnCount)
    , ColumnEvaluator_(std::move(columnEvauator))
{
    YASSERT(KeyColumnCount_ == ColumnEvaluator_->GetKeyColumnCount());

    auto schemaColumnCount = TableSchema_.Columns().size();

    if (columnFilter.All) {
        for (int id = 0; id < schemaColumnCount; ++id) {
            ColumnIds_.push_back(id);
        }
    } else {
        for (int id : columnFilter.Indexes) {
            ColumnIds_.push_back(id);
        }
    }

    ColumnIdToIndex_.resize(schemaColumnCount);
    for (int id = 0; id < schemaColumnCount; ++id) {
        ColumnIdToIndex_[id] = -1;
    }
    for (int index = 0; index < static_cast<int>(ColumnIds_.size()); ++index) {
        int id = ColumnIds_[index];
        if (id >= KeyColumnCount_) {
            ColumnIdToIndex_[id] = index;
        }
    }

    MergedTimestamps_.resize(schemaColumnCount);

    Cleanup();
}

void TSchemafulRowMerger::AddPartialRow(TVersionedRow row)
{
    if (!row)
        return;

    YASSERT(row.GetKeyCount() == KeyColumnCount_);
    YASSERT(row.GetWriteTimestampCount() <= 1);
    YASSERT(row.GetDeleteTimestampCount() <= 1);

    if (!Started_) {
        if (!MergedRow_) {
            MergedRow_ = TUnversionedRow::Allocate(RowBuffer_->GetPool(), ColumnIds_.size());
        }

        const auto* keyBegin = row.BeginKeys();
        for (int index = 0; index < static_cast<int>(ColumnIds_.size()); ++index) {
            int id = ColumnIds_[index];
            auto* mergedValue = &MergedRow_[index];
            if (id < KeyColumnCount_) {
                MergedTimestamps_[index] = MaxTimestamp;
                *mergedValue = keyBegin[id];
            } else {
                MergedTimestamps_[index] = NullTimestamp;
                mergedValue->Id = id;
                mergedValue->Type = EValueType::Null;
                mergedValue->Aggregate = false;
             }
        }

        Started_ = true;
    }

    if (row.GetDeleteTimestampCount() > 0) {
        auto deleteTimestamp = row.BeginDeleteTimestamps()[0];
        LatestDelete_ = std::max(LatestDelete_, deleteTimestamp);
    }

    if (row.GetWriteTimestampCount() > 0) {
        auto writeTimestamp = row.BeginWriteTimestamps()[0];
        LatestWrite_ = std::max(LatestWrite_, writeTimestamp);

        if (writeTimestamp < LatestDelete_) {
            return;
        }

        const auto* partialValuesBegin = row.BeginValues();
        for (int partialIndex = 0; partialIndex < row.GetValueCount(); ++partialIndex) {
            const auto& partialValue = partialValuesBegin[partialIndex];
            if (partialValue.Timestamp > LatestDelete_) {
                int id = partialValue.Id;
                int mergedIndex = ColumnIdToIndex_[id];
                if (mergedIndex >= 0) {
                    if (TableSchema_.Columns()[id].Aggregate) {
                        AggregateValues_.push_back(partialValue);
                    } else if (MergedTimestamps_[mergedIndex] < partialValue.Timestamp) {
                        MergedRow_[mergedIndex] = partialValue;
                        MergedTimestamps_[mergedIndex] = partialValue.Timestamp;
                    }
                }
            }
        }
    }
}

TUnversionedRow TSchemafulRowMerger::BuildMergedRow()
{
    if (!Started_) {
        return TUnversionedRow();
    }

    if (LatestWrite_ == NullTimestamp || LatestWrite_ < LatestDelete_) {
        Cleanup();
        return TUnversionedRow();
    }

    AggregateValues_.erase(
        std::remove_if(
            AggregateValues_.begin(),
            AggregateValues_.end(),
            [latestDelete = LatestDelete_] (const TVersionedValue& value) {
                return value.Timestamp <= latestDelete;
            }),
        AggregateValues_.end());

    std::sort(
        AggregateValues_.begin(),
        AggregateValues_.end(),
        [] (const TVersionedValue& lhs, const TVersionedValue& rhs) {
            return std::tie(lhs.Id, lhs.Timestamp) < std::tie(rhs.Id, rhs.Timestamp);
        });

    AggregateValues_.erase(
        std::unique(
            AggregateValues_.begin(),
            AggregateValues_.end(),
            [] (const TVersionedValue& lhs, const TVersionedValue& rhs) {
                return std::tie(lhs.Id, lhs.Timestamp) == std::tie(rhs.Id, rhs.Timestamp);
            }),
        AggregateValues_.end());

    int begin = 0;
    for (int index = 0; index < AggregateValues_.size(); ++index) {
        if (index == AggregateValues_.size() - 1 || AggregateValues_[begin].Id != AggregateValues_[index + 1].Id) {
            int id = AggregateValues_[begin].Id;
            auto state = MakeUnversionedSentinelValue(EValueType::Null, id);

            for (int valueIndex = index; valueIndex >= begin; --valueIndex) {
                if (!AggregateValues_[valueIndex].Aggregate) {
                    begin = valueIndex;
                }
            }

            for (int valueIndex = begin; valueIndex <= index; ++valueIndex) {
                TUnversionedValue tmpValue;
                ColumnEvaluator_->MergeAggregate(id, &tmpValue, &state, &AggregateValues_[valueIndex], RowBuffer_);
                state = tmpValue;
            }

            state.Aggregate = false;
            auto columnIndex = ColumnIdToIndex_[id];
            MergedTimestamps_[columnIndex] = AggregateValues_[index].Timestamp;
            MergedRow_[columnIndex] = state;
            begin = index + 1;
        }
    }

    for (int index = 0; index < static_cast<int>(ColumnIds_.size()); ++index) {
        int id = ColumnIds_[index];
        if (MergedTimestamps_[index] < LatestDelete_ && !TableSchema_.Columns()[id].Aggregate) {
            MergedRow_[index].Type = EValueType::Null;
        }
    }

    auto mergedRow = MergedRow_;

    Cleanup();
    return mergedRow;
}

void TSchemafulRowMerger::Reset()
{
    YASSERT(!Started_);
    RowBuffer_->Clear();
    MergedRow_ = TUnversionedRow();
}

void TSchemafulRowMerger::Cleanup()
{
    MergedRow_ = TUnversionedRow();
    AggregateValues_.clear();
    LatestWrite_ = NullTimestamp;
    LatestDelete_ = NullTimestamp;
    Started_ = false;
}

DEFINE_REFCOUNTED_TYPE(TSchemafulRowMerger)

////////////////////////////////////////////////////////////////////////////////

TUnversionedRowMerger::TUnversionedRowMerger(
    TRowBufferPtr rowBuffer,
    int keyColumnCount,
    TColumnEvaluatorPtr columnEvauator)
    : RowBuffer_(rowBuffer)
    , TableSchema_(columnEvauator->TableSchema())
    , KeyColumnCount_(keyColumnCount)
    , ColumnEvaluator_(std::move(columnEvauator))
{
    YASSERT(KeyColumnCount_ == ColumnEvaluator_->GetKeyColumnCount());

    ValidValues_.resize(TableSchema_.Columns().size());

    Cleanup();
}

void TUnversionedRowMerger::InitPartialRow(TUnversionedRow row)
{
    if (!Started_) {
        MergedRow_ = TUnversionedRow::Allocate(RowBuffer_->GetPool(), TableSchema_.Columns().size());

        for (int index = 0; index < TableSchema_.Columns().size(); ++index) {
            if (index < KeyColumnCount_) {
                ValidValues_[index] = true;
                MergedRow_[index] = row[index];
            } else {
                ValidValues_[index] = false;
                MergedRow_[index].Id = index;
                MergedRow_[index].Type = EValueType::Null;
                MergedRow_[index].Aggregate = TableSchema_.Columns()[index].Aggregate.HasValue();
            }
        }
    }

    Started_ = true;
}

void TUnversionedRowMerger::AddPartialRow(TUnversionedRow row)
{
    if (!row) {
        return;
    }

    InitPartialRow(row);

    for (int partialIndex = KeyColumnCount_; partialIndex < row.GetCount(); ++partialIndex) {
        auto& partialValue = row[partialIndex];
        int id = partialValue.Id;
        ValidValues_[id] = true;

        if (partialValue.Aggregate) {
            YCHECK(TableSchema_.Columns()[id].Aggregate);
            TUnversionedValue tmpValue;
            ColumnEvaluator_->MergeAggregate(id, &tmpValue, &MergedRow_[id], &partialValue, RowBuffer_);
            tmpValue.Aggregate = MergedRow_[id].Aggregate;
            MergedRow_[id] = tmpValue;
        } else {
            MergedRow_[id] = partialValue;
        }
    }

    Deleted_ = false;
}

void TUnversionedRowMerger::DeletePartialRow(TUnversionedRow row)
{
    // NB: Since we don't have delete timestamps here we need to write Null into all columns.

    InitPartialRow(row);

    for (int index = KeyColumnCount_; index < TableSchema_.Columns().size(); ++index) {
        ValidValues_[index] = true;
        MergedRow_[index].Type = EValueType::Null;
        MergedRow_[index].Aggregate = false;
    }

    Deleted_ = true;
}

TUnversionedRow TUnversionedRowMerger::BuildMergedRow()
{
    if (!Started_) {
        return TUnversionedRow();
    }

    if (Deleted_) {
        auto mergedRow = MergedRow_;
        mergedRow.SetCount(KeyColumnCount_);
        Cleanup();
        return mergedRow;
    }

    bool fullRow = true;
    for (bool validValue : ValidValues_) {
        if (!validValue) {
            fullRow = false;
            break;
        }
    }

    TUnversionedRow mergedRow;

    if (fullRow) {
        mergedRow = MergedRow_;
    } else {
        mergedRow = TUnversionedRow::Allocate(RowBuffer_->GetPool(), TableSchema_.Columns().size());
        int currentIndex = 0;
        for (int index = 0; index < MergedRow_.GetCount(); ++index) {
            if (ValidValues_[index]) {
                mergedRow[currentIndex] = MergedRow_[index];
                ++currentIndex;
            }
        }
        mergedRow.SetCount(currentIndex);
    }

    Cleanup();
    return mergedRow;
}

void TUnversionedRowMerger::Reset()
{
    YASSERT(!Started_);
    RowBuffer_->Clear();
    MergedRow_ = TUnversionedRow();
}

void TUnversionedRowMerger::Cleanup()
{
    MergedRow_ = TUnversionedRow();
    Started_ = false;
}

DEFINE_REFCOUNTED_TYPE(TUnversionedRowMerger)

////////////////////////////////////////////////////////////////////////////////

TVersionedRowMerger::TVersionedRowMerger(
    TRowBufferPtr rowBuffer,
    int keyColumnCount,
    TRetentionConfigPtr config,
    TTimestamp currentTimestamp,
    TTimestamp majorTimestamp,
    TColumnEvaluatorPtr columnEvauator)
    : RowBuffer_(rowBuffer)
    , TableSchema_(columnEvauator->TableSchema())
    , KeyColumnCount_(keyColumnCount)
    , Config_(std::move(config))
    , CurrentTimestamp_(currentTimestamp)
    , MajorTimestamp_(majorTimestamp)
    , ColumnEvaluator_(std::move(columnEvauator))
    , Keys_(KeyColumnCount_)
{
    YASSERT(KeyColumnCount_ == ColumnEvaluator_->GetKeyColumnCount());

    Cleanup();
}

TTimestamp TVersionedRowMerger::GetCurrentTimestamp() const
{
    return CurrentTimestamp_;
}

TTimestamp TVersionedRowMerger::GetMajorTimestamp() const
{
    return MajorTimestamp_;
}

void TVersionedRowMerger::AddPartialRow(TVersionedRow row)
{
    if (!row) {
        return;
    }

    if (!Started_) {
        Started_ = true;
        YASSERT(row.GetKeyCount() == KeyColumnCount_);
        std::copy(row.BeginKeys(), row.EndKeys(), Keys_.data());
    }

    PartialValues_.insert(
        PartialValues_.end(),
        row.BeginValues(),
        row.EndValues());

    DeleteTimestamps_.insert(
        DeleteTimestamps_.end(),
        row.BeginDeleteTimestamps(),
        row.EndDeleteTimestamps());
}

TVersionedRow TVersionedRowMerger::BuildMergedRow()
{
    if (!Started_) {
        return TVersionedRow();
    }

    // Sort delete timestamps in ascending order and remove duplicates.
    std::sort(DeleteTimestamps_.begin(), DeleteTimestamps_.end());
    DeleteTimestamps_.erase(
        std::unique(DeleteTimestamps_.begin(), DeleteTimestamps_.end()),
        DeleteTimestamps_.end());

    // Sort input values by |(id, timestamp)| and remove duplicates.
    std::sort(
        PartialValues_.begin(),
        PartialValues_.end(),
        [] (const TVersionedValue& lhs, const TVersionedValue& rhs) {
            return std::tie(lhs.Id, lhs.Timestamp) < std::tie(rhs.Id, rhs.Timestamp);
        });
    PartialValues_.erase(
        std::unique(
            PartialValues_.begin(),
            PartialValues_.end(),
            [] (const TVersionedValue& lhs, const TVersionedValue& rhs) {
                return std::tie(lhs.Id, lhs.Timestamp) == std::tie(rhs.Id, rhs.Timestamp);
            }),
        PartialValues_.end());

    // Scan through input values.
    auto partialValueIt = PartialValues_.begin();
    while (partialValueIt != PartialValues_.end()) {
        // Extract a range of values for the current column.
        auto columnBeginIt = partialValueIt;
        auto columnEndIt = partialValueIt;
        while (columnEndIt != PartialValues_.end() && columnEndIt->Id == partialValueIt->Id) {
            ++columnEndIt;
        }

        // Merge with delete timestamps and put result into ColumnValues_.
        // Delete timestamps are represented by TheBottom sentinels.
        {
            ColumnValues_.clear();
            auto timestampBeginIt = DeleteTimestamps_.begin();
            auto timestampEndIt = DeleteTimestamps_.end();
            auto columnValueIt = columnBeginIt;
            auto timestampIt = timestampBeginIt;
            while (columnValueIt != columnEndIt || timestampIt != timestampEndIt) {
                if (timestampIt == timestampEndIt ||
                    columnValueIt != columnEndIt && columnValueIt->Timestamp < *timestampIt)
                {
                    ColumnValues_.push_back(*columnValueIt++);
                } else {
                    auto value = MakeVersionedSentinelValue(EValueType::TheBottom, *timestampIt);
                    ColumnValues_.push_back(value);
                    ++timestampIt;
                }
            }
        }

#ifndef NDEBUG
        // Validate merged list.
        for (auto it = ColumnValues_.begin(); it != ColumnValues_.end(); ++it) {
            YASSERT(it + 1 == ColumnValues_.end() || (it->Timestamp <= (it + 1)->Timestamp));
        }
#endif

        // Compute safety limit by MinDataVersions.
        auto safetyEndIt = ColumnValues_.begin();
        if (ColumnValues_.size() > Config_->MinDataVersions) {
            safetyEndIt = ColumnValues_.end() - Config_->MinDataVersions;
        }

        // Adjust safety limit by MinDataTtl.
        while (safetyEndIt != ColumnValues_.begin()) {
            auto timestamp = (safetyEndIt - 1)->Timestamp;
            if (timestamp < CurrentTimestamp_ &&
                TimestampDiffToDuration(timestamp, CurrentTimestamp_).first > Config_->MinDataTtl)
            {
                break;
            }
            --safetyEndIt;
        }

        // Compute retention limit by MaxDataVersions and MaxDataTtl.
        auto retentionBeginIt = safetyEndIt;
        while (retentionBeginIt != ColumnValues_.begin()) {
            if (std::distance(retentionBeginIt, ColumnValues_.end()) >= Config_->MaxDataVersions) {
                break;
            }

            auto timestamp = (retentionBeginIt - 1)->Timestamp;
            if (timestamp < CurrentTimestamp_ &&
                TimestampDiffToDuration(timestamp, CurrentTimestamp_).first > Config_->MaxDataTtl)
            {
                break;
            }

            --retentionBeginIt;
        }

        // For aggregate columns merge values before MajorTimestamp_ and leave other values.
        if (TableSchema_.Columns()[partialValueIt->Id].Aggregate) {
            while (retentionBeginIt != ColumnValues_.begin()
                && retentionBeginIt->Timestamp >= MajorTimestamp_)
            {
                --retentionBeginIt;
            }

            if (retentionBeginIt > ColumnValues_.begin()) {
                int id = partialValueIt->Id;
                auto aggregateBeginIt = ColumnValues_.begin();
                for (auto valueIt = retentionBeginIt; valueIt >= aggregateBeginIt; --valueIt) {
                    if (valueIt->Type == EValueType::TheBottom) {
                        aggregateBeginIt = valueIt + 1;
                    } else if (!valueIt->Aggregate) {
                        aggregateBeginIt = valueIt;
                    }
                }

                if (aggregateBeginIt < retentionBeginIt) {
                    auto state = MakeUnversionedSentinelValue(EValueType::Null, id);

                    for (auto valueIt = aggregateBeginIt; valueIt <= retentionBeginIt; ++valueIt) {
                        TUnversionedValue tmpValue;
                        ColumnEvaluator_->MergeAggregate(id, &tmpValue, &state, &(*valueIt), RowBuffer_);
                        state = tmpValue;
                    }

                    TUnversionedValue& value = *retentionBeginIt;
                    value = state;
                }

            }

            if (retentionBeginIt->Timestamp < MajorTimestamp_) {
                retentionBeginIt->Aggregate = false;
            }
        }

        // Save output values and timestamps.
        for (auto it = ColumnValues_.rbegin(); it.base() != retentionBeginIt; ++it) {
            const auto& value = *it;
            if (value.Type != EValueType::TheBottom) {
                WriteTimestamps_.push_back(value.Timestamp);
                MergedValues_.push_back(value);
            }
        }

        partialValueIt = columnEndIt;
    }

    // Reverse delete timestamps list to make them appear in descending order.
    std::reverse(DeleteTimestamps_.begin(), DeleteTimestamps_.end());

    // Sort write timestamps in descending order, remove duplicates.
    std::sort(WriteTimestamps_.begin(), WriteTimestamps_.end(), std::greater<TTimestamp>());
    WriteTimestamps_.erase(
        std::unique(WriteTimestamps_.begin(), WriteTimestamps_.end()),
        WriteTimestamps_.end());

    // Delete redundant tombstones preceding major timestamp.
    {
        auto earliestWriteTimestamp = WriteTimestamps_.empty()
            ? MaxTimestamp
            : WriteTimestamps_.back();
        auto it = DeleteTimestamps_.begin();
        while (it != DeleteTimestamps_.end() && (*it > earliestWriteTimestamp || *it >= MajorTimestamp_)) {
            ++it;
        }
        DeleteTimestamps_.erase(it, DeleteTimestamps_.end());
    }

    if (MergedValues_.empty() && WriteTimestamps_.empty() && DeleteTimestamps_.empty()) {
        Cleanup();
        return TVersionedRow();
    }

    // Construct output row.
    auto row = TVersionedRow::Allocate(
        RowBuffer_->GetPool(),
        KeyColumnCount_,
        MergedValues_.size(),
        WriteTimestamps_.size(),
        DeleteTimestamps_.size());

    // Construct output keys.
    std::copy(Keys_.begin(), Keys_.end(), row.BeginKeys());

    // Construct output values.
    std::copy(MergedValues_.begin(), MergedValues_.end(), row.BeginValues());

    // Construct output timestamps.
    std::copy(WriteTimestamps_.begin(), WriteTimestamps_.end(), row.BeginWriteTimestamps());
    std::copy(DeleteTimestamps_.begin(), DeleteTimestamps_.end(), row.BeginDeleteTimestamps());

    Cleanup();
    return row;
}

void TVersionedRowMerger::Reset()
{
    YASSERT(!Started_);
    RowBuffer_->Clear();
}

void TVersionedRowMerger::Cleanup()
{
    PartialValues_.clear();
    MergedValues_.clear();
    ColumnValues_.clear();

    WriteTimestamps_.clear();
    DeleteTimestamps_.clear();

    Started_ = false;
}

DEFINE_REFCOUNTED_TYPE(TVersionedRowMerger)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

