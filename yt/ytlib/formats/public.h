#pragma once

#include <core/misc/public.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TYsonFormatConfig)
DECLARE_REFCOUNTED_CLASS(TJsonFormatConfig)
DECLARE_REFCOUNTED_CLASS(TTableFormatConfigBase)
DECLARE_REFCOUNTED_CLASS(TYamrFormatConfig)
DECLARE_REFCOUNTED_CLASS(TYamrFormatConfigBase)
DECLARE_REFCOUNTED_CLASS(TDsvFormatConfig)
DECLARE_REFCOUNTED_CLASS(TDsvFormatConfigBase)
DECLARE_REFCOUNTED_CLASS(TYamredDsvFormatConfig)
DECLARE_REFCOUNTED_CLASS(TSchemafulDsvFormatConfig)

DECLARE_REFCOUNTED_STRUCT(IYamrConsumer)

DECLARE_REFCOUNTED_STRUCT(ISchemalessFormatWriter)

DECLARE_REFCOUNTED_CLASS(TShemalessWriterForDsv)
DECLARE_REFCOUNTED_CLASS(TShemalessWriterForYamr)
DECLARE_REFCOUNTED_CLASS(TShemalessWriterForYamredDsv)
DECLARE_REFCOUNTED_CLASS(TShemalessWriterForSchemafulDsv)

struct IParser;

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
