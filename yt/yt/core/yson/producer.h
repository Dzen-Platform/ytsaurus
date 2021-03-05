#pragma once

#include "public.h"
#include "string.h"

#include <yt/yt/core/actions/callback.h>

#include <yt/yt/core/misc/property.h>

namespace NYT::NYson {

////////////////////////////////////////////////////////////////////////////////

//! A callback capable of generating YSON by calling appropriate
//! methods for its IYsonConsumer argument.
using TYsonCallback = TCallback<void(IYsonConsumer*)>;

////////////////////////////////////////////////////////////////////////////////

//! A TYsonCallback annotated with type.
class TYsonProducer
{
public:
    DEFINE_BYVAL_RO_PROPERTY(NYson::EYsonType, Type);

public:
    TYsonProducer();
    TYsonProducer(
        TYsonCallback callback,
        EYsonType type = NYson::EYsonType::Node);

    void Run(IYsonConsumer* consumer) const;

private:
    const TYsonCallback Callback_;

};

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TYsonProducer& value, NYson::IYsonConsumer* consumer);
void Serialize(const TYsonCallback& value, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYson
