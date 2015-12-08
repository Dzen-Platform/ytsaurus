#include "stdafx.h"
#include "string.h"
#include "stream.h"
#include "null_consumer.h"
#include "parser.h"
#include "consumer.h"

#include <yt/core/misc/serialize.h>

namespace NYT {
namespace NYson {

////////////////////////////////////////////////////////////////////////////////

TYsonString::TYsonString()
    : Type_(EYsonType::None)
{ }

TYsonString::TYsonString(const Stroka& data, EYsonType type)
    : Data_(data)
    , Type_(type)
{ }

void TYsonString::Validate() const
{
    TStringInput input(Data());
    ParseYson(TYsonInput(&input, GetType()), GetNullYsonConsumer());
}

void TYsonString::Save(TStreamSaveContext& context) const
{
    using NYT::Save;
    switch (Type_) {
        case EYsonType::None:
            Save(context, Stroka());
            break;

        case EYsonType::Node:
            Save(context, Data_);
            break;

        default:
            YUNREACHABLE();
    }
}

void TYsonString::Load(TStreamLoadContext& context)
{
    NYT::Load(context, Data_);
    Type_ = Data_.empty() ? EYsonType::None : EYsonType::Node;
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TYsonString& yson, IYsonConsumer* consumer)
{
    consumer->OnRaw(yson);
}

bool operator == (const TYsonString& lhs, const TYsonString& rhs)
{
    return lhs.Data() == rhs.Data() && lhs.GetType() == rhs.GetType();
}

bool operator != (const TYsonString& lhs, const TYsonString& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
