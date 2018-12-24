#include "rowset.h"

#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/schema.h>
#include <yt/client/table_client/unversioned_writer.h>
#include <yt/client/table_client/unversioned_row.h>

#include <yt/client/table_client/wire_protocol.h>

#include <yt/core/actions/future.h>

namespace NYT::NApi {

using namespace NTabletClient;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

template <class TRow>
class TRowset
    : public IRowset<TRow>
{
public:
    TRowset(
        const TTableSchema& schema,
        TSharedRange<TRow> rows)
        : Schema_(schema)
        , Rows_(std::move(rows))
    { }

    virtual const TTableSchema& Schema() const override
    {
        return Schema_;
    }

    virtual TRange<TRow> GetRows() const override
    {
        return Rows_;
    }

private:
    const TTableSchema Schema_;
    const TSharedRange<TRow> Rows_;
};

IUnversionedRowsetPtr CreateRowset(
    const TTableSchema& schema,
    TSharedRange<TUnversionedRow> rows)
{
    return New<TRowset<TUnversionedRow>>(schema, std::move(rows));
}

IVersionedRowsetPtr CreateRowset(
    const TTableSchema& schema,
    TSharedRange<TVersionedRow> rows)
{
    return New<TRowset<TVersionedRow>>(schema, std::move(rows));
}

////////////////////////////////////////////////////////////////////////////////

class TSchemafulRowsetWriter
    : public IUnversionedRowset
    , public IUnversionedRowsetWriter
{
public:
    explicit TSchemafulRowsetWriter(const TTableSchema& schema)
        : Schema_(schema)
    { }

    virtual const TTableSchema& Schema() const override
    {
        return Schema_;
    }

    virtual TRange<TUnversionedRow> GetRows() const override
    {
        return MakeRange(Rows_);
    }

    TFuture<IUnversionedRowsetPtr> GetResult() const
    {
        return Result_.ToFuture();
    }

    virtual TFuture<void> Close() override
    {
        Result_.Set(IUnversionedRowsetPtr(this));
        Result_.Reset();
        return VoidFuture;
    }

    virtual bool Write(TRange<TUnversionedRow> rows) override
    {
        for (auto row : rows) {
            Rows_.push_back(RowBuffer_->Capture(row));
        }
        return true;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return VoidFuture;
    }

private:
    const TTableSchema Schema_;

    TPromise<IUnversionedRowsetPtr> Result_ = NewPromise<IUnversionedRowsetPtr>();

    struct TSchemafulRowsetWriterBufferTag
    { };

    const TRowBufferPtr RowBuffer_ = New<TRowBuffer>(TSchemafulRowsetWriterBufferTag());
    std::vector<TUnversionedRow> Rows_;

};

std::tuple<IUnversionedRowsetWriterPtr, TFuture<IUnversionedRowsetPtr>> CreateSchemafulRowsetWriter(const TTableSchema& schema)
{
    auto writer = New<TSchemafulRowsetWriter>(schema);
    return std::make_tuple(writer, writer->GetResult());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi

