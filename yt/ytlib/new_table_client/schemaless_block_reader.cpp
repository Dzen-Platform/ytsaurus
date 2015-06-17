#include "stdafx.h"

#include "schemaless_block_reader.h"
#include "private.h"

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

THorizontalSchemalessBlockReader::THorizontalSchemalessBlockReader(
    const TSharedRef& block,
    const NProto::TBlockMeta& meta,
    const std::vector<int>& idMapping,
    int keyColumnCount)
    : Block_(block)
    , Meta_(meta)
    , IdMapping_(idMapping)
    , KeyColumnCount_(keyColumnCount)
{
    YCHECK(Meta_.row_count() > 0);

    // Allocate space for key.
    std::vector<TUnversionedValue> key(
        KeyColumnCount_,
        MakeUnversionedSentinelValue(EValueType::Null, 0));
    Key_ = TOwningKey(key.data(), key.data() + KeyColumnCount_);

    i64 offsetsLength = sizeof(ui32) * Meta_.row_count();

    Offsets_ = TRef(Block_.Begin(), Block_.Begin() + offsetsLength);
    Data_ = TRef(Offsets_.End(), Block_.End());

    JumpToRowIndex(0);
}

bool THorizontalSchemalessBlockReader::NextRow()
{
    return JumpToRowIndex(RowIndex_ + 1);
}

bool THorizontalSchemalessBlockReader::SkipToRowIndex(i64 rowIndex)
{
    YCHECK(rowIndex >= RowIndex_);
    return JumpToRowIndex(rowIndex);
}
    
bool THorizontalSchemalessBlockReader::SkipToKey(const TOwningKey& key)
{
    if (GetKey() >= key) {
        // We are already further than pivot key.
        return true;
    }

    auto index = LowerBound(
        RowIndex_,
        Meta_.row_count(),
        [&] (i64 index) {
            YCHECK(JumpToRowIndex(index));
            return GetKey() < key;
        });

    return JumpToRowIndex(index);
}

const TOwningKey& THorizontalSchemalessBlockReader::GetKey() const
{
    return Key_;
}

TUnversionedRow THorizontalSchemalessBlockReader::GetRow(TChunkedMemoryPool* memoryPool)
{
    TUnversionedRow row = TUnversionedRow::Allocate(memoryPool, ValueCount_);
    int valueCount = 0;
    for (int i = 0; i < ValueCount_; ++i) {
        TUnversionedValue value;
        CurrentPointer_ += ReadValue(CurrentPointer_, &value);

        if (IdMapping_[value.Id] >= 0) {
            value.Id = IdMapping_[value.Id];
            row[valueCount] = value;
            ++valueCount;
        }
    }
    row.GetHeader()->Count = valueCount;
    return row;
}

i64 THorizontalSchemalessBlockReader::GetRowIndex() const
{
    return RowIndex_;
}

bool THorizontalSchemalessBlockReader::JumpToRowIndex(i64 rowIndex)
{
    if (rowIndex >= Meta_.row_count()) {
        return false;
    }

    RowIndex_ = rowIndex;

    ui32 offset = *reinterpret_cast<const ui32*>(Offsets_.Begin() + rowIndex * sizeof(ui32));
    CurrentPointer_ = Data_.Begin() + offset;

    CurrentPointer_ += ReadVarUint32(CurrentPointer_, &ValueCount_);
    YCHECK(ValueCount_ >= KeyColumnCount_);

    const char* ptr = CurrentPointer_;
    for (int i = 0; i < KeyColumnCount_; ++i) {
        ptr += ReadValue(ptr, const_cast<TUnversionedValue*>(Key_.Begin()) + i);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
