#include "stdafx.h"
#include "format.h"

#include "json_parser.h"
#include "json_writer.h"

#include "dsv_parser.h"
#include "dsv_writer.h"

#include "yamr_parser.h"
#include "yamr_writer.h"

#include "yamred_dsv_parser.h"
#include "yamred_dsv_writer.h"

#include "schemaful_dsv_parser.h"
#include "schemaful_dsv_writer.h"

#include "schemaless_writer_adapter.h"

#include "yson_parser.h"
#include "json_writer.h"
#include "schemaful_writer.h"

#include <core/misc/error.h>

#include <core/yson/writer.h>
#include <core/yson/forwarding_consumer.h>

#include <core/ytree/fluent.h>

namespace NYT {
namespace NFormats {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TFormat::TFormat()
    : Type_(EFormatType::Null)
{ }

TFormat::TFormat(EFormatType type, const IAttributeDictionary* attributes)
    : Type_(type)
    , Attributes_(attributes ? attributes->Clone() : CreateEphemeralAttributes())
{ }

TFormat::TFormat(const TFormat& other)
    : Type_(other.Type_)
    , Attributes_(other.Attributes_->Clone())
{ }

TFormat& TFormat::operator=(const TFormat& other)
{
    if (this != &other) {
        Type_ = other.Type_;
        Attributes_ = other.Attributes_ ? other.Attributes_->Clone() : nullptr;
    }
    return *this;
}

const IAttributeDictionary& TFormat::Attributes() const
{
    return *Attributes_;
}

void Serialize(const TFormat& value, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginAttributes()
            .Items(value.Attributes())
        .EndAttributes()
        .Value(value.GetType());
}

void Deserialize(TFormat& value, INodePtr node)
{
    if (node->GetType() != ENodeType::String) {
        THROW_ERROR_EXCEPTION("Format name must be a string");
    }

    auto typeStr = node->GetValue<Stroka>();
    EFormatType type;
    try {
        type = ParseEnum<EFormatType>(typeStr);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Invalid format name %Qv",
            typeStr);
    }

    value = TFormat(type, &node->Attributes());
}

///////////////////////////////////////////////////////////////////////////////

EYsonType DataTypeToYsonType(EDataType dataType)
{
    switch (dataType) {
        case EDataType::Structured:
            return EYsonType::Node;
        case EDataType::Tabular:
            return EYsonType::ListFragment;
        default:
            THROW_ERROR_EXCEPTION("Data type %Qv is not supported by YSON",
                dataType);
    }
}

std::unique_ptr<IYsonConsumer> CreateConsumerForYson(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    auto config = ConvertTo<TYsonFormatConfigPtr>(&attributes);
    auto ysonType = DataTypeToYsonType(dataType);
    auto enableRaw = (config->Format == EYsonFormat::Binary);
    
    return std::unique_ptr<IYsonConsumer>(new TYsonWriter(
        output,
        config->Format,
        ysonType,
        enableRaw,
        config->BooleanAsString));
}

std::unique_ptr<IYsonConsumer> CreateConsumerForJson(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    auto config = ConvertTo<TJsonFormatConfigPtr>(&attributes);
    return CreateJsonConsumer(output, DataTypeToYsonType(dataType), config);
}

std::unique_ptr<IYsonConsumer> CreateConsumerForDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TOutputStream* output)
{
    auto config = ConvertTo<TDsvFormatConfigPtr>(&attributes);
    switch (dataType) {
        case EDataType::Structured:
            return std::unique_ptr<IYsonConsumer>(new TDsvNodeConsumer(output, config));

        case EDataType::Tabular:
        case EDataType::Binary:
        case EDataType::Null:
            THROW_ERROR_EXCEPTION("Data type %Qv is not supported by DSV",
                dataType);

        default:
            YUNREACHABLE();
    };
}

std::unique_ptr<IYsonConsumer> CreateConsumerForFormat(
    const TFormat& format,
    EDataType dataType,
    TOutputStream* output)
{
    switch (format.GetType()) {
        case EFormatType::Yson:
            return CreateConsumerForYson(dataType, format.Attributes(), output);
        case EFormatType::Json:
            return CreateConsumerForJson(dataType, format.Attributes(), output);
        case EFormatType::Dsv:
            return CreateConsumerForDsv(dataType, format.Attributes(), output);
        default:
            THROW_ERROR_EXCEPTION("Unsupported output format %Qlv",
                format.GetType());
    }
}

////////////////////////////////////////////////////////////////////////////////

ISchemafulWriterPtr CreateSchemafulWriterForYson(
    const IAttributeDictionary& attributes,
    const TTableSchema& schema,
    IAsyncOutputStreamPtr output)
{
    auto config = ConvertTo<TYsonFormatConfigPtr>(&attributes);

    return New<TSchemafulWriter>(output, schema, [&] (TOutputStream* buffer) {
        return std::make_unique<NYson::TYsonWriter>(buffer, config->Format, EYsonType::ListFragment);
    });
}

ISchemafulWriterPtr CreateSchemafulWriterForJson(
    const IAttributeDictionary& attributes,
    const TTableSchema& schema,
    IAsyncOutputStreamPtr output)
{
    auto config = ConvertTo<TJsonFormatConfigPtr>(&attributes);

    return New<TSchemafulWriter>(output, schema, [&] (TOutputStream* buffer) {
        return CreateJsonConsumer(buffer, EYsonType::ListFragment, config);
    });
}

ISchemafulWriterPtr CreateSchemafulWriterForSchemafulDsv(
    const IAttributeDictionary& attributes,
    const TTableSchema& schema,
    IAsyncOutputStreamPtr output)
{
    auto config = ConvertTo<TSchemafulDsvFormatConfigPtr>(&attributes);
    return CreateSchemafulWriterForSchemafulDsv(output, schema, config);
}

ISchemafulWriterPtr CreateSchemafulWriterForFormat(
    const TFormat& format,
    const TTableSchema& schema,
    IAsyncOutputStreamPtr output)
{
    switch (format.GetType()) {
        case EFormatType::Yson:
            return CreateSchemafulWriterForYson(format.Attributes(), schema, output);
        case EFormatType::Json:
            return CreateSchemafulWriterForJson(format.Attributes(), schema, output);
        case EFormatType::SchemafulDsv:
            return CreateSchemafulWriterForSchemafulDsv(format.Attributes(), schema, output);
        default:
            THROW_ERROR_EXCEPTION("Unsupported output format %Qlv",
                format.GetType());
    }
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessFormatWriterPtr CreateSchemalessWriterForDsv(
    const IAttributeDictionary& attributes,
    TNameTablePtr nameTable,
    NConcurrency::IAsyncOutputStreamPtr output,
    bool enableContextSaving,
    TControlAttributesConfigPtr controlAttributesConfig,
    int /* keyColumnCount */)
{
    if (controlAttributesConfig->EnableKeySwitch) {
        THROW_ERROR_EXCEPTION("Key switches are not supported in DSV format");
    }

    if (controlAttributesConfig->EnableRangeIndex) {
        THROW_ERROR_EXCEPTION("Range indices are not supported in DSV format");
    }

    if (controlAttributesConfig->EnableRowIndex) {
        THROW_ERROR_EXCEPTION("Row indices are not supported in DSV format");
    }

    auto config = ConvertTo<TDsvFormatConfigPtr>(&attributes);
    return New<TSchemalessWriterForDsv>(
        nameTable, 
        enableContextSaving,
        controlAttributesConfig, 
        output, 
        config);
}

ISchemalessFormatWriterPtr CreateSchemalessWriterForYamr(
    const IAttributeDictionary& attributes,
    TNameTablePtr nameTable,
    NConcurrency::IAsyncOutputStreamPtr output,
    bool enableContextSaving,
    TControlAttributesConfigPtr controlAttributesConfig,
    int keyColumnCount)
{
    auto config = ConvertTo<TYamrFormatConfigPtr>(&attributes);
    if (controlAttributesConfig->EnableKeySwitch && !config->Lenval) {
        THROW_ERROR_EXCEPTION("Key switches are not supported in text YAMR format");
    }

    if (controlAttributesConfig->EnableRangeIndex && !config->Lenval) {
        THROW_ERROR_EXCEPTION("Range indices are not supported in text YAMR format");
    }

    if (controlAttributesConfig->EnableRowIndex && !config->Lenval) {
         THROW_ERROR_EXCEPTION("Row indices are not supported in text YAMR format");
    }

    return New<TSchemalessWriterForYamr>(
        nameTable, 
        output, 
        enableContextSaving, 
        controlAttributesConfig,
        keyColumnCount, 
        config);
}

ISchemalessFormatWriterPtr CreateSchemalessWriterForYamredDsv(
    const IAttributeDictionary& attributes,
    TNameTablePtr nameTable,
    NConcurrency::IAsyncOutputStreamPtr output,
    bool enableContextSaving,
    TControlAttributesConfigPtr controlAttributesConfig,
    int keyColumnCount)
{
    auto config = ConvertTo<TYamredDsvFormatConfigPtr>(&attributes);
    if (controlAttributesConfig->EnableKeySwitch && !config->Lenval) {
        THROW_ERROR_EXCEPTION("Key switches are not supported in text YAMRed DSV format");
    }

    if (controlAttributesConfig->EnableRangeIndex && !config->Lenval) {
        THROW_ERROR_EXCEPTION("Range indices are not supported in text YAMRed DSV format");
    }

    if (controlAttributesConfig->EnableRowIndex && !config->Lenval) {
         THROW_ERROR_EXCEPTION("Row indices are not supported in text YAMRed DSV format");
    }

    return New<TSchemalessWriterForYamredDsv>(
        nameTable, 
        output, 
        enableContextSaving, 
        controlAttributesConfig, 
        keyColumnCount, 
        config);
}

ISchemalessFormatWriterPtr CreateSchemalessWriterForSchemafulDsv(
    const IAttributeDictionary& attributes,
    TNameTablePtr nameTable,
    NConcurrency::IAsyncOutputStreamPtr output,
    bool enableContextSaving,
    TControlAttributesConfigPtr controlAttributesConfig,
    int /* keyColumnCount */)
{
    if (controlAttributesConfig->EnableKeySwitch) {
        THROW_ERROR_EXCEPTION("Key switches are not supported in schemaful DSV format");
    }

    if (controlAttributesConfig->EnableRangeIndex) {
        THROW_ERROR_EXCEPTION("Range indices are not supported in schemaful DSV format");
    }

    if (controlAttributesConfig->EnableRowIndex) {
         THROW_ERROR_EXCEPTION("Row indices are not supported in schemaful DSV format");
    }

    if (controlAttributesConfig->EnableTableIndex) {
        THROW_ERROR_EXCEPTION("Table indices are not supported in schemaful DSV format");
    }

    auto config = ConvertTo<TSchemafulDsvFormatConfigPtr>(&attributes);
    if (!config->Columns) {
        THROW_ERROR_EXCEPTION("Config must contain columns for schemaful DSV schemaless writer");
    }

    return New<TSchemalessWriterForSchemafulDsv>(
        nameTable, 
        output, 
        enableContextSaving,
        controlAttributesConfig, 
        config);
}

ISchemalessFormatWriterPtr CreateSchemalessWriterForFormat(
    const TFormat& format,
    TNameTablePtr nameTable,
    NConcurrency::IAsyncOutputStreamPtr output,
    bool enableContextSaving,
    TControlAttributesConfigPtr controlAttributesConfig,
    int keyColumnCount)
{
    switch (format.GetType()) {
        case EFormatType::Dsv:
            return CreateSchemalessWriterForDsv(
                format.Attributes(),
                nameTable,
                std::move(output),
                enableContextSaving,
                controlAttributesConfig,
                keyColumnCount);
        case EFormatType::Yamr:
            return CreateSchemalessWriterForYamr(
                format.Attributes(),
                nameTable,
                std::move(output),
                enableContextSaving,
                controlAttributesConfig,
                keyColumnCount);
        case EFormatType::YamredDsv:
            return CreateSchemalessWriterForYamredDsv(
                format.Attributes(),
                nameTable,
                std::move(output),
                enableContextSaving,
                controlAttributesConfig,
                keyColumnCount);
        case EFormatType::SchemafulDsv:
            return CreateSchemalessWriterForSchemafulDsv(
                format.Attributes(),
                nameTable,
                std::move(output),
                enableContextSaving,
                controlAttributesConfig,
                keyColumnCount); 
        default:
            auto adapter = New<TSchemalessWriterAdapter>(
                nameTable,
                std::move(output),
                enableContextSaving,
                controlAttributesConfig,
                keyColumnCount);
            adapter->Init(format);
            return adapter;
    }
}

////////////////////////////////////////////////////////////////////////////////

TYsonProducer CreateProducerForDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Tabular) {
        THROW_ERROR_EXCEPTION("DSV is supported only for tabular data");
    }
    auto config = ConvertTo<TDsvFormatConfigPtr>(&attributes);
    return BIND([=] (IYsonConsumer* consumer) {
        ParseDsv(input, consumer, config);
    });
}

TYsonProducer CreateProducerForYamr(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Tabular) {
        THROW_ERROR_EXCEPTION("YAMR is supported only for tabular data");
    }
    auto config = ConvertTo<TYamrFormatConfigPtr>(&attributes);
    return BIND([=] (IYsonConsumer* consumer) {
        ParseYamr(input, consumer, config);
    });
}

TYsonProducer CreateProducerForYamredDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Tabular) {
        THROW_ERROR_EXCEPTION("Yamred DSV is supported only for tabular data");
    }
    auto config = ConvertTo<TYamredDsvFormatConfigPtr>(&attributes);
    return BIND([=] (IYsonConsumer* consumer) {
        ParseYamredDsv(input, consumer, config);
    });
}

TYsonProducer CreateProducerForSchemafulDsv(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    if (dataType != EDataType::Tabular) {
        THROW_ERROR_EXCEPTION("Schemaful DSV is supported only for tabular data");
    }
    auto config = ConvertTo<TSchemafulDsvFormatConfigPtr>(&attributes);
    return BIND([=] (IYsonConsumer* consumer) {
        ParseSchemafulDsv(input, consumer, config);
    });
}

TYsonProducer CreateProducerForJson(
    EDataType dataType,
    const IAttributeDictionary& attributes,
    TInputStream* input)
{
    auto ysonType = DataTypeToYsonType(dataType);
    auto config = ConvertTo<TJsonFormatConfigPtr>(&attributes);
    return BIND([=] (IYsonConsumer* consumer) {
        ParseJson(input, consumer, config, ysonType);
    });
}

TYsonProducer CreateProducerForYson(EDataType dataType, TInputStream* input)
{
    auto ysonType = DataTypeToYsonType(dataType);
    return ConvertToProducer(TYsonInput(input, ysonType));
}

TYsonProducer CreateProducerForFormat(const TFormat& format, EDataType dataType, TInputStream* input)
{
    switch (format.GetType()) {
        case EFormatType::Yson:
            return CreateProducerForYson(dataType, input);
        case EFormatType::Json:
            return CreateProducerForJson(dataType, format.Attributes(), input);
        case EFormatType::Dsv:
            return CreateProducerForDsv(dataType, format.Attributes(), input);
        case EFormatType::Yamr:
            return CreateProducerForYamr(dataType, format.Attributes(), input);
        case EFormatType::YamredDsv:
            return CreateProducerForYamredDsv(dataType, format.Attributes(), input);
        case EFormatType::SchemafulDsv:
            return CreateProducerForSchemafulDsv(dataType, format.Attributes(), input);
        default:
            THROW_ERROR_EXCEPTION("Unsupported input format %Qlv",
                format.GetType());
    }
}

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IParser> CreateParserForFormat(const TFormat& format, EDataType dataType, IYsonConsumer* consumer)
{
    switch (format.GetType()) {
        case EFormatType::Yson:
            return CreateParserForYson(consumer, DataTypeToYsonType(dataType));
        case EFormatType::Json: {
            auto config = ConvertTo<TJsonFormatConfigPtr>(&format.Attributes());
            return std::unique_ptr<IParser>(new TJsonParser(consumer, config, DataTypeToYsonType(dataType)));
        }
        case EFormatType::Dsv: {
            auto config = ConvertTo<TDsvFormatConfigPtr>(&format.Attributes());
            return CreateParserForDsv(consumer, config);
        }
        case EFormatType::Yamr: {
            auto config = ConvertTo<TYamrFormatConfigPtr>(&format.Attributes());
            return CreateParserForYamr(consumer, config);
        }
        case EFormatType::YamredDsv: {
            auto config = ConvertTo<TYamredDsvFormatConfigPtr>(&format.Attributes());
            return CreateParserForYamredDsv(consumer, config);
        }
        case EFormatType::SchemafulDsv: {
            auto config = ConvertTo<TSchemafulDsvFormatConfigPtr>(&format.Attributes());
            return CreateParserForSchemafulDsv(consumer, config);
        }
        default:
            THROW_ERROR_EXCEPTION("Unsupported input format %Qlv",
                format.GetType());
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
