#include "helpers.h"

#include "escape.h"
#include "format.h"

namespace NYT {
namespace NFormats {

using namespace NTableClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TFormatsConsumerBase::TFormatsConsumerBase()
    : Parser(this)
{ }

void TFormatsConsumerBase::OnRaw(const TStringBuf& yson, EYsonType type)
{
    Parser.Parse(yson, type);
}

void TFormatsConsumerBase::Flush()
{ }

////////////////////////////////////////////////////////////////////////////////

bool IsSpecialJsonKey(const TStringBuf& key)
{
    return key.size() > 0 && key[0] == '$';
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
void WriteInt(T value, TOutputStream* output)
{
    char buf[64];
    char* end = buf + 64;
    char* start = WriteIntToBufferBackwards(end, value);
    output->Write(start, end - start);
}

void WriteDouble(double value, TOutputStream* output)
{
    char buf[64];
    char* begin = buf;
    auto length = FloatToString(value, buf, sizeof(buf));
    if (std::find(begin, begin + length, '.') == begin + length &&
        std::find(begin, begin + length, 'e') == begin + length)
    {
        begin[length++] = '.';
    }
    output->Write(begin, length);
}

void WriteUnversionedValue(const TUnversionedValue& value, TOutputStream* output, const TEscapeTable& escapeTable)
{
    switch (value.Type) {
        case EValueType::Null:
            break;
        case EValueType::Int64:
            WriteInt(value.Data.Int64, output);
            break;
        case EValueType::Uint64:
            WriteInt(value.Data.Uint64, output);
            break;
        case EValueType::Double:
            WriteDouble(value.Data.Double, output);
            break;
        case EValueType::Boolean:
            output->Write(FormatBool(value.Data.Boolean));
            break;
        case EValueType::String:
            EscapeAndWrite(TStringBuf(value.Data.String, value.Length), output, escapeTable);
            break;
        default:
            THROW_ERROR_EXCEPTION("Values of type %v are not supported by the chosen format", value.Type)
                << TErrorAttribute("value", ToString(value));
            break;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
