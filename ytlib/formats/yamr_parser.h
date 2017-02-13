#pragma once

#include "public.h"
#include "config.h"

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IParser> CreateParserForYamr(
    NYson::IYsonConsumer* consumer,
    TYamrFormatConfigPtr config = New<TYamrFormatConfig>());

////////////////////////////////////////////////////////////////////////////////

void ParseYamr(
    TInputStream* input,
    NYson::IYsonConsumer* consumer,
    TYamrFormatConfigPtr config = New<TYamrFormatConfig>());

void ParseYamr(
    const TStringBuf& data,
    NYson::IYsonConsumer* consumer,
    TYamrFormatConfigPtr config = New<TYamrFormatConfig>());

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
