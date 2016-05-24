#pragma once

#include "public.h"

#include <yt/ytlib/object_client/public.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

NObjectClient::TObjectId GetObjectIdFromDataSplit(const TDataSplit& dataSplit);

TTableSchema GetTableSchemaFromDataSplit(const TDataSplit& dataSplit);

TKeyColumns GetKeyColumnsFromDataSplit(const TDataSplit& dataSplit);

TOwningKey GetLowerBoundFromDataSplit(const TDataSplit& dataSplit);

TOwningKey GetUpperBoundFromDataSplit(const TDataSplit& dataSplit);

TKeyRange GetBothBoundsFromDataSplit(const TDataSplit& dataSplit);

TTimestamp GetTimestampFromDataSplit(const TDataSplit& dataSplit);

bool IsSorted(const TDataSplit& dataSplit);

void SetObjectId(TDataSplit* dataSplit, const NObjectClient::TObjectId& objectId);

void SetTableSchema(TDataSplit* dataSplit, const TTableSchema& tableSchema);

void SetKeyColumns(TDataSplit* dataSplit, const TKeyColumns& keyColumns);

void SetLowerBound(TDataSplit* dataSplit, const TOwningKey & lowerBound);

void SetUpperBound(TDataSplit* dataSplit, const TOwningKey & upperBound);

void SetBothBounds(TDataSplit* dataSplit, const TKeyRange& keyRange);

void SetTimestamp(TDataSplit* dataSplit, TTimestamp timestamp);

// XXX(sandello): For testing purposes only.
void SetSorted(TDataSplit* dataSplit, bool isSorted);

TKeyColumns TableSchemaToKeyColumns(const TTableSchema& schema, size_t keySize);

//! Computes key index for a given column name.
int ColumnNameToKeyPartIndex(const TKeyColumns& keyColumns, const Stroka& columnName);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

