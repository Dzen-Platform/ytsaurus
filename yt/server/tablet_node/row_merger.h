#pragma once

#include "public.h"

#include <core/misc/small_vector.h>

#include <ytlib/new_table_client/versioned_row.h>
#include <ytlib/new_table_client/unversioned_row.h>

#include <ytlib/api/public.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TSchemafulRowMerger
{
public:
    TSchemafulRowMerger(
        TChunkedMemoryPool* pool,
        int schemaColumnCount,
        int keyColumnCount,
        const TColumnFilter& columnFilter);

    void AddPartialRow(TVersionedRow row);
    TUnversionedRow BuildMergedRow();
    void Reset();

private:
    TChunkedMemoryPool* Pool_;
    int SchemaColumnCount_;
    int KeyColumnCount_;

    NVersionedTableClient::TUnversionedRow MergedRow_;
    SmallVector<NVersionedTableClient::TTimestamp, NVersionedTableClient::TypicalColumnCount> MergedTimestamps_;

    SmallVector<int, NVersionedTableClient::TypicalColumnCount> ColumnIds_;
    SmallVector<int, NVersionedTableClient::TypicalColumnCount> ColumnIdToIndex_;
    
    TTimestamp LatestWrite_;
    TTimestamp LatestDelete_;
    bool Started_ = false;


    void Cleanup();

};

////////////////////////////////////////////////////////////////////////////////

class TVersionedRowMerger
{
public:
    TVersionedRowMerger(
        TChunkedMemoryPool* pool,
        int keyColumnCount,
        TRetentionConfigPtr config,
        TTimestamp currentTimestamp,
        TTimestamp majorTimestamp);

    void AddPartialRow(TVersionedRow row);
    TVersionedRow BuildMergedRow();
    void Reset();

private:
    TChunkedMemoryPool* Pool_;
    int KeyColumnCount_;
    TRetentionConfigPtr Config_;
    TTimestamp CurrentTimestamp_;
    TTimestamp MajorTimestamp_;

    bool Started_;
    SmallVector<TUnversionedValue, NVersionedTableClient::TypicalColumnCount> Keys_;

    std::vector<TVersionedValue> PartialValues_;
    std::vector<TVersionedValue> ColumnValues_;
    std::vector<TVersionedValue> MergedValues_;

    std::vector<TTimestamp> WriteTimestamps_;
    std::vector<TTimestamp> DeleteTimestamps_;

    void Cleanup();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
