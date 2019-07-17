#include "skiff_parser.h"
#include "skiff_yson_converter.h"

#include "helpers.h"
#include "parser.h"
#include "yson_map_to_unversioned_value.h"

#include <yt/core/skiff/schema_match.h>
#include <yt/core/skiff/parser.h>

#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/table_consumer.h>
#include <yt/client/table_client/value_consumer.h>

#include <yt/core/concurrency/coroutine.h>

#include <yt/core/yson/parser.h>

#include <util/generic/strbuf.h>
#include <util/stream/zerocopy.h>
#include <util/stream/buffer.h>

namespace NYT::NFormats {

using namespace NTableClient;
using namespace NSkiff;

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////

using TSkiffToUnversionedValueConverter = std::function<void(TCheckedInDebugSkiffParser*, IValueConsumer*)>;

template<EWireType wireType, bool required>
class TSimpleTypeConverterImpl
{
public:
    explicit TSimpleTypeConverterImpl(ui16 columnId, TYsonToUnversionedValueConverter* ysonConverter = nullptr)
        : ColumnId_(columnId)
        , YsonConverter_(ysonConverter)
    {}

    void operator()(TCheckedInDebugSkiffParser* parser, IValueConsumer* valueConsumer)
    {
        if constexpr (!required) {
            ui8 tag = parser->ParseVariant8Tag();
            if (tag == 0) {
                valueConsumer->OnValue(MakeUnversionedNullValue(ColumnId_));
                return;
            } else if (tag > 1) {
                const auto name = valueConsumer->GetNameTable()->GetName(ColumnId_);
                THROW_ERROR_EXCEPTION(
                    "Found bad variant8 tag %Qv when parsing optional field %Qv",
                    tag,
                    name);
            }
        }
        if constexpr (wireType == EWireType::Yson32) {
            YT_VERIFY(YsonConverter_);
            auto ysonString = parser->ParseYson32();
            YsonConverter_->SetColumnIndex(ColumnId_);
            YsonConverter_->SetValueConsumer(valueConsumer);
            ParseYsonStringBuffer(ysonString, NYson::EYsonType::Node, YsonConverter_);
        } else if constexpr (wireType == EWireType::Int64) {
            valueConsumer->OnValue(MakeUnversionedInt64Value(parser->ParseInt64(), ColumnId_));
        } else if constexpr (wireType == EWireType::Uint64) {
            valueConsumer->OnValue(MakeUnversionedUint64Value(parser->ParseUint64(), ColumnId_));
        } else if constexpr (wireType == EWireType::Double) {
            valueConsumer->OnValue(MakeUnversionedDoubleValue(parser->ParseDouble(), ColumnId_));
        } else if constexpr (wireType == EWireType::Boolean) {
            valueConsumer->OnValue(MakeUnversionedBooleanValue(parser->ParseBoolean(), ColumnId_));
        } else if constexpr (wireType == EWireType::String32) {
            valueConsumer->OnValue(MakeUnversionedStringValue(parser->ParseString32(), ColumnId_));
        } else {
            static_assert(wireType == EWireType::Int64);
        }
    }

private:
    const ui16 ColumnId_;
    TYsonToUnversionedValueConverter* YsonConverter_ = nullptr;
};

////////////////////////////////////////////////////////////////////////////////

TSkiffToUnversionedValueConverter CreateSimpleValueConverter(
    EWireType wireType,
    bool required,
    ui16 columnId,
    TYsonToUnversionedValueConverter* ysonConverter)
{
    switch (wireType) {
#define CASE(x) \
        case x: \
            do { \
                if (required) { \
                    return TSimpleTypeConverterImpl<x, true>(columnId, ysonConverter); \
                } else { \
                    return TSimpleTypeConverterImpl<x, false>(columnId, ysonConverter); \
                } \
            } while (0)

        CASE(EWireType::Int64);
        CASE(EWireType::Uint64);
        CASE(EWireType::Boolean);
        CASE(EWireType::Double);
        CASE(EWireType::String32);
        CASE(EWireType::Yson32);
#undef CASE

        default:
            YT_ABORT();
    }
}

class TComplexValueConverter
{
public:
    TComplexValueConverter(TSkiffToYsonConverter converter, ui16 columnId)
        : Converter_(std::move(converter))
        , ColumnId_(columnId)
    { }

    void operator() (TCheckedInDebugSkiffParser* parser, IValueConsumer* valueConsumer)
    {
        Buffer_.Clear();
        {
            TBufferOutput out(Buffer_);
            NYson::TBufferedBinaryYsonWriter ysonWriter(&out);
            Converter_(parser, &ysonWriter);
            ysonWriter.Flush();
        }
        auto value = TStringBuf(Buffer_.Data(), Buffer_.Size());
        valueConsumer->OnValue(MakeUnversionedAnyValue(value, ColumnId_));
    }

private:
    const TSkiffToYsonConverter Converter_;
    const ui16 ColumnId_;
    TBuffer Buffer_;
};

TSkiffToUnversionedValueConverter CreateComplexValueConverter(
    NTableClient::TComplexTypeFieldDescriptor descriptor,
    const TSkiffSchemaPtr& skiffSchema,
    ui16 columnId,
    bool sparseColumn)
{
    TSkiffToYsonConverterConfig config;
    config.AllowOmitTopLevelOptional = sparseColumn;
    auto converter = CreateSkiffToYsonConverter(std::move(descriptor), skiffSchema, config);
    return TComplexValueConverter(converter, columnId);
}

////////////////////////////////////////////////////////////////////////////////

class TSkiffParserImpl
{
public:
    TSkiffParserImpl(IValueConsumer* valueConsumer, const TSkiffSchemaPtr& skiffSchema)
        : SkiffSchemaList_({skiffSchema})
        , ValueConsumer_(valueConsumer)
        , OtherColumnsConsumer_(ValueConsumer_)
    {
        THashMap<TString, const TColumnSchema*> columnSchemas;
        for (const auto& column : valueConsumer->GetSchema().Columns()) {
            columnSchemas[column.Name()] = &column;
        }

        auto genericTableDescriptions = CreateTableDescriptionList(
            SkiffSchemaList_, RangeIndexColumnName, RowIndexColumnName);

        for (const auto& genericTableDescription : genericTableDescriptions) {
            auto& parserTableDescription = TableDescriptions_.emplace_back();
            parserTableDescription.HasOtherColumns = genericTableDescription.HasOtherColumns;
            for (const auto& fieldDescription : genericTableDescription.DenseFieldDescriptionList) {
                const auto columnId = ValueConsumer_->GetNameTable()->GetIdOrRegisterName(fieldDescription.Name());
                TSkiffToUnversionedValueConverter converter;
                auto columnSchema = columnSchemas.FindPtr(fieldDescription.Name());
                if (columnSchema && !(*columnSchema)->SimplifiedLogicalType()) {
                    converter = CreateComplexValueConverter(
                        TComplexTypeFieldDescriptor(fieldDescription.Name(), (*columnSchema)->LogicalType()),
                        fieldDescription.Schema(),
                        columnId,
                        /*sparseColumn*/ false);
                } else {
                    converter = CreateSimpleValueConverter(
                        fieldDescription.ValidatedSimplify(),
                        fieldDescription.IsRequired(),
                        columnId,
                        &YsonToUnversionedValueConverter_
                    );
                }
                parserTableDescription.DenseFieldConverters.emplace_back(converter);
            }

            for (const auto& fieldDescription : genericTableDescription.SparseFieldDescriptionList) {
                const auto columnId = ValueConsumer_->GetNameTable()->GetIdOrRegisterName(fieldDescription.Name());
                TSkiffToUnversionedValueConverter converter;
                auto columnSchema = columnSchemas.FindPtr(fieldDescription.Name());
                if (columnSchema && !(*columnSchema)->SimplifiedLogicalType()) {
                    converter = CreateComplexValueConverter(
                        TComplexTypeFieldDescriptor(fieldDescription.Name(), (*columnSchema)->LogicalType()),
                        fieldDescription.Schema(),
                        columnId,
                        /*sparseColumn*/ true);
                } else {
                    converter = CreateSimpleValueConverter(
                        fieldDescription.ValidatedSimplify(),
                        fieldDescription.IsRequired(),
                        columnId,
                        &YsonToUnversionedValueConverter_
                    );
                }
                parserTableDescription.SparseFieldConverters.emplace_back(converter);
            }
        }
    }

    void DoParse(IZeroCopyInput* stream)
    {
        Parser_ = std::make_unique<TCheckedInDebugSkiffParser>(CreateVariant16Schema(SkiffSchemaList_), stream);

        while (Parser_->HasMoreData()) {
            auto tag = Parser_->ParseVariant16Tag();
            if (tag >= TableDescriptions_.size()) {
                THROW_ERROR_EXCEPTION("Unknown table index variant16 tag")
                        << TErrorAttribute("tag", tag);
            }

            if (tag > 0) {
                THROW_ERROR_EXCEPTION("Unkwnown table index varint16 tag %v",
                    tag);
            }
            ValueConsumer_->OnBeginRow();

            for (const auto& converter : TableDescriptions_[tag].DenseFieldConverters) {
                converter(Parser_.get(), ValueConsumer_);
            }

            if (!TableDescriptions_[tag].SparseFieldConverters.empty()) {
                for (auto sparseFieldIdx = Parser_->ParseVariant16Tag();
                     sparseFieldIdx != EndOfSequenceTag<ui16>();
                     sparseFieldIdx = Parser_->ParseVariant16Tag()) {
                    if (sparseFieldIdx >= TableDescriptions_[tag].SparseFieldConverters.size()) {
                        THROW_ERROR_EXCEPTION("Bad sparse field index %Qv, total sparse field count %Qv",
                            sparseFieldIdx,
                            TableDescriptions_[tag].SparseFieldConverters.size());
                    }

                    const auto& converter = TableDescriptions_[tag].SparseFieldConverters[sparseFieldIdx];
                    converter(Parser_.get(), ValueConsumer_);
                }
            }

            if (TableDescriptions_[tag].HasOtherColumns) {
                auto buf = Parser_->ParseYson32();
                ParseYsonStringBuffer(
                    buf,
                    NYson::EYsonType::Node,
                    &OtherColumnsConsumer_);
            }

            ValueConsumer_->OnEndRow();
        }
    }

    ui64 GetReadBytesCount()
    {
        return Parser_->GetReadBytesCount();
    }

private:
    struct TTableDescription
    {
        std::vector<TSkiffToUnversionedValueConverter> DenseFieldConverters;
        std::vector<TSkiffToUnversionedValueConverter> SparseFieldConverters;
        bool HasOtherColumns = false;
    };

    TSkiffSchemaList SkiffSchemaList_;

    IValueConsumer* ValueConsumer_;
    TYsonToUnversionedValueConverter YsonToUnversionedValueConverter_;
    TYsonMapToUnversionedValueConverter OtherColumnsConsumer_;

    std::unique_ptr<TCheckedInDebugSkiffParser> Parser_;
    std::vector<TTableDescription> TableDescriptions_;
};

////////////////////////////////////////////////////////////////////////////////

class TSkiffPushParser
    : public IParser
{
public:
    TSkiffPushParser(const TSkiffSchemaPtr& skiffSchema, IValueConsumer* consumer)
        : ParserImpl_(std::make_unique<TSkiffParserImpl>(consumer, skiffSchema))
        , ParserCoroPipe_(
            BIND(
                [=](IZeroCopyInput* stream) {
                    ParserImpl_->DoParse(stream);
                }))
    {}

    void Read(TStringBuf data) override
    {
        if (!data.empty()) {
            ParserCoroPipe_.Feed(data);
        }
    }

    void Finish() override
    {
        ParserCoroPipe_.Finish();
    }

private:
    std::unique_ptr<TSkiffParserImpl> ParserImpl_;
    TCoroPipe ParserCoroPipe_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace // anonymous

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IParser> CreateParserForSkiff(
    IValueConsumer* consumer,
    TSkiffFormatConfigPtr config,
    int tableIndex)
{
    auto skiffSchemas = ParseSkiffSchemas(config->SkiffSchemaRegistry, config->TableSkiffSchemas);
    if (tableIndex >= static_cast<int>(skiffSchemas.size())) {
        THROW_ERROR_EXCEPTION("Skiff format config does not describe table #%v",
            tableIndex);
    }
    return CreateParserForSkiff(
        skiffSchemas[tableIndex],
        consumer);
}

std::unique_ptr<IParser> CreateParserForSkiff(
    TSkiffSchemaPtr skiffSchema,
    IValueConsumer* consumer)
{
    auto tableDescriptionList = CreateTableDescriptionList({skiffSchema}, RangeIndexColumnName, RowIndexColumnName);
    if (tableDescriptionList.size() != 1) {
        THROW_ERROR_EXCEPTION("Expected to have single table, actual table description count %Qv",
            tableDescriptionList.size());
    }
    return std::make_unique<TSkiffPushParser>(
        skiffSchema,
        consumer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFormats
