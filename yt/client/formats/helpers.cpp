#include "helpers.h"

#include "escape.h"
#include "format.h"

#include <yt/core/misc/format.h>

namespace NYT::NFormats {

using namespace NTableClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TFormatsConsumerBase::TFormatsConsumerBase()
    : Parser(this)
{ }

void TFormatsConsumerBase::OnRaw(TStringBuf yson, EYsonType type)
{
    Parser.Parse(yson, type);
}

void TFormatsConsumerBase::Flush()
{ }

////////////////////////////////////////////////////////////////////////////////

template <class T>
void WriteInt(T value, IOutputStream* output)
{
    char buf[64];
    char* end = buf + 64;
    char* start = WriteIntToBufferBackwards(end, value);
    output->Write(start, end - start);
}

void WriteDouble(double value, IOutputStream* output)
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

void WriteUnversionedValue(const TUnversionedValue& value, IOutputStream* output, const TEscapeTable& escapeTable)
{
    switch (value.Type) {
        case EValueType::Null:
            return;
        case EValueType::Int64:
            WriteInt(value.Data.Int64, output);
            return;
        case EValueType::Uint64:
            WriteInt(value.Data.Uint64, output);
            return;
        case EValueType::Double:
            WriteDouble(value.Data.Double, output);
            return;
        case EValueType::Boolean:
            output->Write(FormatBool(value.Data.Boolean));
            return;
        case EValueType::String:
            EscapeAndWrite(TStringBuf(value.Data.String, value.Length), output, escapeTable);
            return;

        case EValueType::Any:
        case EValueType::Composite:

        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            break;
    }
    THROW_ERROR_EXCEPTION("Values of type %Qlv are not supported by the chosen format", value.Type)
        << TErrorAttribute("value", ToString(value));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFormats
