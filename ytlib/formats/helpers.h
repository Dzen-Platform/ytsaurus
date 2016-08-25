#pragma once

#include "public.h"

#include <yt/core/yson/consumer.h>
#include <yt/core/yson/parser.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

class TFormatsConsumerBase
    : public virtual NYson::IFlushableYsonConsumer
{
public:
    TFormatsConsumerBase();

    // This method has standard implementation for YAMR, DSV and YAMRED DSV formats.
    virtual void OnRaw(const TStringBuf& yson, NYson::EYsonType type) override;

    virtual void Flush() override;

private:
    NYson::TStatelessYsonParser Parser;
};

////////////////////////////////////////////////////////////////////////////////

bool IsSpecialJsonKey(const TStringBuf& str);

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
