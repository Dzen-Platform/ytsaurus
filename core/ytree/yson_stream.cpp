#include "yson_stream.h"

#include <yt/core/misc/common.h>

#include <yt/core/yson/parser.h>

namespace NYT {
namespace NYTree {

using namespace NYson;

static const size_t ParseBufferSize = 1 << 16;

////////////////////////////////////////////////////////////////////////////////

TYsonInput::TYsonInput(TInputStream* stream, EYsonType type)
    : Stream_(stream)
    , Type_(type)
{ }

////////////////////////////////////////////////////////////////////////////////

TYsonOutput::TYsonOutput(TOutputStream* stream, EYsonType type)
    : Stream_(stream)
    , Type_(type)
{ }

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TYsonInput& input, IYsonConsumer* consumer)
{
    ParseYson(input, consumer);
}

void ParseYson(
    const TYsonInput& input,
    IYsonConsumer* consumer,
    bool enableLinePositionInfo)
{
    TYsonParser parser(consumer, input.GetType(), enableLinePositionInfo);
    char buffer[ParseBufferSize];
    while (true) {
        size_t bytesRead = input.GetStream()->Read(buffer, ParseBufferSize);
        if (bytesRead == 0) {
            break;
        }
        parser.Read(TStringBuf(buffer, bytesRead));
    }
    parser.Finish();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
