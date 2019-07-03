#include "block_output_stream.h"

#include "table.h"
#include "db_helpers.h"
#include "helpers.h"

#include <yt/ytlib/table_client/schemaless_chunk_writer.h>

#include <yt/client/table_client/unversioned_writer.h>
#include <yt/client/table_client/schema.h>
#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/row_buffer.h>

#include <yt/core/concurrency/scheduler.h>

#include <DataTypes/DataTypeFactory.h>

namespace NYT::NClickHouseServer {

using namespace NTableClient;
using namespace NConcurrency;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

bool IsScalarType(EValueType valueType)
{
    return
        valueType == EValueType::Int64 ||
        valueType == EValueType::Uint64 ||
        valueType == EValueType::Double ||
        valueType == EValueType::Boolean ||
        valueType == EValueType::String;
}

////////////////////////////////////////////////////////////////////////////////

class TBlockOutputStream
    : public DB::IBlockOutputStream
{
public:
    TBlockOutputStream(IUnversionedWriterPtr writer, const TLogger& logger)
        : Writer_(std::move(writer))
        , RowBuffer_(New<TRowBuffer>())
        , Schema_(Writer_->GetSchema())
        , NameTable_(Writer_->GetNameTable())
        , Logger(logger)
    {
        for (const auto& column : Schema_.Columns()) {
            if (!IsScalarType(column.GetPhysicalType())) {
                if (column.Required()) {
                    THROW_ERROR_EXCEPTION(
                        "Column %v is not of primitive type and is required; CHYT currently is not able to deal with "
                        "non-primitive columns",
                        column.Name())
                            << TErrorAttribute("type", column.GetPhysicalType());
                }
                continue;
            } else {
                auto clickHouseColumn = TClickHouseColumn::FromColumnSchema(column);
                const auto& dataType = DB::DataTypeFactory::instance().get(GetTypeName(*clickHouseColumn));
                YT_VERIFY(clickHouseColumn);
                Header_.insert({ dataType->createColumn(), dataType, column.Name() });
                PositionToId_.emplace_back(NameTable_->GetIdOrRegisterName(column.Name()));
            }
        }
        YT_LOG_INFO("Output stream header formed (ColumnCount: %v)", Header_.columns());
    }

    virtual DB::Block getHeader() const override
    {
        return Header_;
    }

    virtual void write(const DB::Block& block) override
    {
        YT_LOG_TRACE("Writing block (RowCount: %v, ColumnCount: %v, ByteCount: %v)", block.rows(), block.columns(), block.bytes());
        std::vector<TUnversionedRow> rows;
        std::vector<DB::Field> fields;
        fields.reserve(block.rows() * block.columns());
        for (int rowIndex = 0; rowIndex < static_cast<int>(block.rows()); ++rowIndex) {
            auto row = RowBuffer_->AllocateUnversioned(block.columns());
            for (int columnIndex = 0; columnIndex < static_cast<int>(block.columns()); ++columnIndex) {
                const auto& column = block.getByPosition(columnIndex).column;
                auto& field = fields.emplace_back();
                column->get(rowIndex, field);
                auto& value = row[columnIndex];
                value.Id = PositionToId_[columnIndex];
                if (field.isNull()) {
                    if (Schema_.Columns()[columnIndex].Required()) {
                        THROW_ERROR_EXCEPTION("Value NULL is not allowed in required column %v", Schema_.Columns()[columnIndex].Name());
                    }
                    value.Type = EValueType::Null;
                } else {
                    value.Type = Schema_.Columns()[columnIndex].GetPhysicalType();
                    ConvertToUnversionedValue(field, &value);
                }
            }
            rows.emplace_back(row);
        }

        if (!Writer_->Write(rows)) {
            WaitFor(Writer_->GetReadyEvent())
                .ThrowOnError();
        }
        RowBuffer_->Clear();
    }

    virtual void writeSuffix() override
    {
        YT_LOG_INFO("Closing writer");
        WaitFor(Writer_->Close())
            .ThrowOnError();
        YT_LOG_INFO("Writer closed");
    }

private:
    IUnversionedWriterPtr Writer_;
    TRowBufferPtr RowBuffer_;
    TTableSchema Schema_;
    TNameTablePtr NameTable_;
    TLogger Logger;
    std::vector<int> PositionToId_;
    DB::Block Header_;
};

////////////////////////////////////////////////////////////////////////////////

DB::BlockOutputStreamPtr CreateBlockOutputStream(IUnversionedWriterPtr writer, const TLogger& logger)
{
    return std::make_shared<TBlockOutputStream>(std::move(writer), logger);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
