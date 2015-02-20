#pragma once

#include <core/misc/intrusive_ptr.h>

#include <vector>

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

class TLogConfig;
typedef TIntrusivePtr<TLogConfig> TLogConfigPtr;

struct ILogWriter;
typedef TIntrusivePtr<ILogWriter> ILogWriterPtr;
typedef std::vector<ILogWriterPtr> ILogWriters;

class TLogger;
class TLogManager;

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
