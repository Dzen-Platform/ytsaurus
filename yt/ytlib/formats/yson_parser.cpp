#include "stdafx.h"
#include "parser.h"
#include "yson_parser.h"

#include <ytlib/new_table_client/public.h>

#include <core/yson/parser.h>

namespace NYT {
namespace NFormats {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

//! Wrapper around YSON parser that implements IParser interface.
class TYsonParserAdapter
    : public IParser
{
public:
    TYsonParserAdapter(
        IYsonConsumer* consumer,
        EYsonType type,
        bool enableLinePositionInfo)
        : Parser(consumer, type, enableLinePositionInfo, NVersionedTableClient::MaxRowWeightLimit)
    { }

    virtual void Read(const TStringBuf& data) override
    {
        Parser.Read(data);
    }

    virtual void Finish() override
    {
        Parser.Finish();
    }

private:
    TYsonParser Parser;

};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IParser> CreateParserForYson(
    IYsonConsumer* consumer,
    EYsonType type,
    bool enableLinePositionInfo)
{
    return std::unique_ptr<IParser>(new TYsonParserAdapter(consumer, type, enableLinePositionInfo));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
