#include "dynamic_store_bits.h"
#include "automaton.h"
#include "tablet.h"

namespace NYT {
namespace NTabletNode {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

#ifndef _win_

const int TSortedDynamicRow::PrimaryLockIndex;
const ui32 TSortedDynamicRow::PrimaryLockMask;
const ui32 TSortedDynamicRow::AllLocksMask;

#endif

////////////////////////////////////////////////////////////////////////////////

TOwningKey RowToKey(
    const TTableSchema& schema,
    TSortedDynamicRow row)
{
    TUnversionedOwningRowBuilder builder;
    ui32 nullKeyBit = 1;
    ui32 nullKeyMask = row.GetNullKeyMask();
    const auto* srcKey = row.BeginKeys();
    auto columnIt = schema.Columns().begin();
    for (int index = 0;
         index < schema.GetKeyColumnCount();
         ++index, nullKeyBit <<= 1, ++srcKey, ++columnIt)
    {
        TUnversionedValue dstKey;
        dstKey.Id = index;
        if (nullKeyMask & nullKeyBit) {
            dstKey.Type = EValueType::Null;
        } else {
            dstKey.Type = columnIt->Type;
            if (IsStringLikeType(EValueType(dstKey.Type))) {
                dstKey.Length = srcKey->String->Length;
                dstKey.Data.String = srcKey->String->Data;
            } else {
                ::memcpy(&dstKey.Data, srcKey, sizeof(TDynamicValueData));
            }
        }
        builder.AddValue(dstKey);
    }
    return builder.FinishRow();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
