#pragma once

#include "name_table.h"
#include "value_consumer.h"

#include <contrib/testing/gmock.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TMockValueConsumer
    : public TValueConsumerBase
{
public:
    TMockValueConsumer(
        TNameTablePtr nameTable,
        bool allowUnknownColumns,
        const TTableSchema& schema = TTableSchema(),
        const TTypeConversionConfigPtr& typeConversionConfig = New<TTypeConversionConfig>())
        : TValueConsumerBase(schema, typeConversionConfig)
        , NameTable_(nameTable)
        , AllowUnknowsColumns_(allowUnknownColumns)
    {
        InitializeIdToTypeMapping();
    }

    MOCK_METHOD0(OnBeginRow, void());
    MOCK_METHOD1(OnMyValue, void(const TUnversionedValue& value));
    MOCK_METHOD0(OnEndRow, void());

    virtual TNameTablePtr GetNameTable() const override
    {
        return NameTable_;
    }

    virtual bool GetAllowUnknownColumns() const override
    {
        return AllowUnknowsColumns_;
    }

private:
    TNameTablePtr NameTable_;
    bool AllowUnknowsColumns_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
