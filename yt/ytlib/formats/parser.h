#pragma once

#include "public.h"

#include <yt/core/misc/public.h>

#include <yt/core/yson/public.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

struct IParser
    : public TNonCopyable
{
    virtual ~IParser()
    { }

    virtual void Read(const TStringBuf& data) = 0;
    virtual void Finish() = 0;
};

////////////////////////////////////////////////////////////////////////////////

void Parse(TInputStream* input, IParser* parser);

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
