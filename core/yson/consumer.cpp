#include "consumer.h"
#include "string.h"
#include "parser.h"

namespace NYT {
namespace NYson {

////////////////////////////////////////////////////////////////////////////////

void IYsonConsumer::OnRaw(const TYsonString& yson)
{
    OnRaw(yson.GetData(), yson.GetType());
}

void IYsonConsumer::OnRaw(const TSharedRefArray& refArray, EYsonType type)
{
    ParseYsonSharedRefArray(refArray, type, this);
}

////////////////////////////////////////////////////////////////////////////////

void TYsonConsumerBase::OnRaw(const TStringBuf& str, EYsonType type)
{
    ParseYsonStringBuffer(str, type, this);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
