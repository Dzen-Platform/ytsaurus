#include "yamr_parser.h"
#include "yamr_base_parser.h"

#include <yt/core/misc/common.h>
#include <yt/core/misc/error.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

namespace {

class TYamrParserConsumer
    : public TYamrConsumerBase
{
public:
    TYamrParserConsumer(NYson::IYsonConsumer* consumer, TYamrFormatConfigPtr config)
        : TYamrConsumerBase(consumer)
        , Config(config)
    { }

    virtual void ConsumeKey(const TStringBuf& key) override
    {
        Consumer->OnListItem();
        Consumer->OnBeginMap();
        Consumer->OnKeyedItem(Config->Key);
        Consumer->OnStringScalar(key);
    }

    virtual void ConsumeSubkey(const TStringBuf& subkey) override
    {
        Consumer->OnKeyedItem(Config->Subkey);
        Consumer->OnStringScalar(subkey);
    }

    virtual void ConsumeValue(const TStringBuf& value) override
    {
        Consumer->OnKeyedItem(Config->Value);
        Consumer->OnStringScalar(value);
        Consumer->OnEndMap();
    }

private:
    TYamrFormatConfigPtr Config;

};

} // namespace

///////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IParser> CreateParserForYamr(
    NYson::IYsonConsumer* consumer,
    TYamrFormatConfigPtr config)
{
    if (!config) {
        config = New<TYamrFormatConfig>();
    }

    auto parserConsumer = New<TYamrParserConsumer>(consumer, config);

    return config->Lenval
        ? std::unique_ptr<IParser>(
            new TYamrLenvalBaseParser(
                parserConsumer,
                config->HasSubkey))
        : std::unique_ptr<IParser>(
            new TYamrDelimitedBaseParser(
                parserConsumer,
                config->HasSubkey,
                config->FieldSeparator,
                config->RecordSeparator,
                config->EnableEscaping, // Enable key escaping
                config->EnableEscaping, // Enable value escaping
                config->EscapingSymbol));
}

///////////////////////////////////////////////////////////////////////////////

void ParseYamr(
    TInputStream* input,
    NYson::IYsonConsumer* consumer,
    TYamrFormatConfigPtr config)
{
    auto parser = CreateParserForYamr(consumer, config);
    Parse(input, parser.get());
}

void ParseYamr(
    const TStringBuf& data,
    NYson::IYsonConsumer* consumer,
    TYamrFormatConfigPtr config)
{
    auto parser = CreateParserForYamr(consumer, config);
    parser->Read(data);
    parser->Finish();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
