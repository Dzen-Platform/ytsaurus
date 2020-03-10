#include "consumer.h"
#include "string.h"
#include "parser.h"

namespace NYT::NYson {

////////////////////////////////////////////////////////////////////////////////

void IYsonConsumer::OnRaw(const TYsonString& yson)
{
    OnRaw(yson.GetData(), yson.GetType());
}

void IYsonConsumer::OnRaw(const TYsonStringBuf& yson)
{
    OnRaw(yson.GetData(), yson.GetType());
}

////////////////////////////////////////////////////////////////////////////////

void TYsonConsumerBase::OnRaw(TStringBuf str, EYsonType type)
{
    ParseYsonStringBuffer(str, type, this);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYson
