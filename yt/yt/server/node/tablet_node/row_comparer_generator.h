#pragma once

#include "dynamic_store_bits.h"

#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/library/codegen/function.h>

////////////////////////////////////////////////////////////////////////////////

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

typedef int(TDDComparerSignature)(ui32, const TDynamicValueData*, ui32, const TDynamicValueData*);
typedef int(TDUComparerSignature)(ui32, const TDynamicValueData*, const TUnversionedValue*, int);
typedef int(TUUComparerSignature)(const TUnversionedValue*, i32, const TUnversionedValue*, i32);

////////////////////////////////////////////////////////////////////////////////

std::tuple<
    NCodegen::TCGFunction<TDDComparerSignature>,
    NCodegen::TCGFunction<TDUComparerSignature>,
    NCodegen::TCGFunction<TUUComparerSignature>>
GenerateComparers(int keyColumnCount, const TTableSchema& schema);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
