#pragma once

#include <yt/core/misc/enum.h>

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

// Any changes to this enum must be also propagated to FormatLevel.
DEFINE_ENUM(ELogLevel,
    (Minimum)
    (Trace)
    (Debug)
    (Info)
    (Warning)
    (Error)
    (Fatal)
    (Maximum)
);

DEFINE_ENUM(EWriterType,
    (File)
    (Stdout)
    (Stderr)
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TLogConfig)
DECLARE_REFCOUNTED_CLASS(TWriterConfig)
DECLARE_REFCOUNTED_CLASS(TRuleConfig)

struct TLogEvent;
class TLogger;
class TLogManager;

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
