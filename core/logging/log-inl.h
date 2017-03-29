#pragma once
#ifndef LOG_INL_H_
#error "Direct inclusion of this file is not allowed, include log.h"
#endif
#undef LOG_INL_H_

#include <yt/core/profiling/timing.h>

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

template <class... TArgs>
TLogger& TLogger::AddTag(const char* format, const TArgs&... args)
{
    return AddRawTag(Format(format, args...));
}

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <class... TArgs>
Stroka FormatLogMessage(const char* format, const TArgs&... args)
{
    return Format(format, args...);
}

template <class... TArgs>
Stroka FormatLogMessage(const TError& error, const char* format, const TArgs&... args)
{
    TStringBuilder builder;
    Format(&builder, format, args...);
    builder.AppendChar('\n');
    builder.AppendString(ToString(error));
    return builder.Flush();
}

template <class T>
Stroka FormatLogMessage(const T& obj)
{
    return ToString(obj);
}

template <class TLogger>
void LogEventImpl(
    TLogger& logger,
    ELogLevel level,
    Stroka message)
{
    TLogEvent event;
    event.Instant = NProfiling::GetCpuInstant();
    event.Category = logger.GetCategory();
    event.Level = level;
    event.Message = std::move(message);
    event.ThreadId = TThread::CurrentThreadId();
    event.FiberId = NConcurrency::GetCurrentFiberId();
    event.TraceId = NTracing::GetCurrentTraceContext().GetTraceId();
    logger.Write(std::move(event));
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
