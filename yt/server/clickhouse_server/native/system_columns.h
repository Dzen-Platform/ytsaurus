#pragma once

#include "public.h"

#include "table_schema.h"

#include <util/generic/maybe.h>

namespace NYT::NClickHouseServer::NNative {

////////////////////////////////////////////////////////////////////////////////

struct TSystemColumns
{
    TMaybe<TString> TableName;

    size_t GetCount() const
    {
        return TableName.Defined() ? 1 : 0;
    }

    TColumnList ToColumnList() const
    {
        TColumnList columns;
        columns.reserve(GetCount());

        if (TableName.Defined()) {
            columns.emplace_back(*TableName, EClickHouseColumnType::String);
        }

        return columns;
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer::NNative

