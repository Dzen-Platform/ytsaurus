#pragma once

#include "public.h"
#include "config.h"
#include "helpers.h"
#include "schemaful_dsv_table.h"

#include <core/misc/blob.h>
#include <core/misc/nullable.h>

#include <core/concurrency/async_stream.h>

#include <ytlib/table_client/public.h>
#include <ytlib/table_client/schemaful_writer.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ESchemafulDsvConsumerState,
    (None)
    (ExpectValue)
    (ExpectAttributeName)
    (ExpectAttributeValue)
    (ExpectEndAttributes)
    (ExpectEntity)
);

//! Note: only tabular format is supported.
class TSchemafulDsvConsumer
    : public virtual TFormatsConsumerBase
{
public:
    explicit TSchemafulDsvConsumer(
        TOutputStream* stream,
        TSchemafulDsvFormatConfigPtr config = New<TSchemafulDsvFormatConfig>());

    // IYsonConsumer overrides.
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
    using EState = ESchemafulDsvConsumerState;

    TOutputStream* Stream_;
    TSchemafulDsvFormatConfigPtr Config_;

    TSchemafulDsvTable Table_;

    std::set<TStringBuf> Keys_;
    std::map<TStringBuf, TStringBuf> Values_;

    std::vector<Stroka> ValueHolder_;

    int ValueCount_ = 0;
    TStringBuf CurrentKey_;

    int TableIndex_ = 0;

    EState State_ = EState::None;

    NTableClient::EControlAttribute ControlAttribute_;

    void WriteRow();
    void EscapeAndWrite(const TStringBuf& value) const;
};

////////////////////////////////////////////////////////////////////////////////

// This class contains methods common for SchemafulDsvWriter and SchemalessWriterForSchemafulDsv.
class TSchemafulDsvWriterBase
{
protected:
    void WriteValue(const NTableClient::TUnversionedValue& value);
    
    void WriteRaw(const TStringBuf& str);
    void WriteRaw(char ch);
    
    TBlob Buffer_;

private:
    static char* WriteInt64Reversed(char* ptr, i64 value);
    static char* WriteUint64Reversed(char* ptr, ui64 value);    
};

////////////////////////////////////////////////////////////////////////////////

class TSchemafulDsvWriter
    : public NTableClient::ISchemafulWriter
    , public TSchemafulDsvWriterBase
{
public:
    TSchemafulDsvWriter(
        NConcurrency::IAsyncOutputStreamPtr stream,
        std::vector<int> columnIdMapping,
        TSchemafulDsvFormatConfigPtr config = New<TSchemafulDsvFormatConfig>());

    virtual TFuture<void> Close() override;

    virtual bool Write(const std::vector<NTableClient::TUnversionedRow>& rows) override;

    virtual TFuture<void> GetReadyEvent() override;


private:

    NConcurrency::IAsyncOutputStreamPtr Stream_;
    std::vector<int> ColumnIdMapping_;

    TSchemafulDsvFormatConfigPtr Config_;

    TFuture<void> Result_;
};

////////////////////////////////////////////////////////////////////////////////

NTableClient::ISchemafulWriterPtr CreateSchemafulDsvWriter(
    NConcurrency::IAsyncOutputStreamPtr stream,
    const NTableClient::TTableSchema& schema,
    TSchemafulDsvFormatConfigPtr config = New<TSchemafulDsvFormatConfig>());

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT

