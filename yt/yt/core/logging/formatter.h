#pragma once

#include "config.h"
#include "pattern.h"

#include <yt/core/misc/ref_counted.h>

namespace NYT::NLogging {

////////////////////////////////////////////////////////////////////////////////

class TCachingDateFormatter
{
public:
    TCachingDateFormatter();

    const char* Format(NProfiling::TCpuInstant instant);

private:
    void Update(NProfiling::TCpuInstant instant);

    TMessageBuffer Cached_;
    NProfiling::TCpuInstant Deadline_ = 0;
    NProfiling::TCpuInstant Liveline_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct ILogFormatter
{
    virtual ~ILogFormatter() = default;
    virtual i64 WriteFormatted(IOutputStream* outputStream, const TLogEvent& event) const = 0;
    virtual void WriteLogReopenSeparator(IOutputStream* outputStream) const = 0;
    virtual void WriteLogStartEvent(IOutputStream* outputStream) const = 0;
    virtual void WriteLogSkippedEvent(IOutputStream* outputStream, i64 count, TStringBuf skippedBy) const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TPlainTextLogFormatter
    : public ILogFormatter
{
public:
    TPlainTextLogFormatter();

    virtual i64  WriteFormatted(IOutputStream* outputStream, const TLogEvent& event) const override;
    virtual void WriteLogReopenSeparator(IOutputStream* outputStream) const override;
    virtual void WriteLogStartEvent(IOutputStream* outputStream) const override;
    virtual void WriteLogSkippedEvent(IOutputStream* outputStream, i64 count, TStringBuf skippedBy) const override;

private:
    const std::unique_ptr<TMessageBuffer> Buffer_;
    const std::unique_ptr<TCachingDateFormatter> CachingDateFormatter_;
};

////////////////////////////////////////////////////////////////////////////////

class TJsonLogFormatter
    : public ILogFormatter
{
public:
    TJsonLogFormatter(const THashMap<TString, NYTree::INodePtr>& commonFields);

    virtual i64 WriteFormatted(IOutputStream* outputStream, const TLogEvent& event) const override;
    virtual void WriteLogReopenSeparator(IOutputStream* outputStream) const override;
    virtual void WriteLogStartEvent(IOutputStream* outputStream) const override;
    virtual void WriteLogSkippedEvent(IOutputStream* outputStream, i64 count, TStringBuf skippedBy) const override;

private:
    const std::unique_ptr<TCachingDateFormatter> CachingDateFormatter_;
    const THashMap<TString, NYTree::INodePtr> CommonFields_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLogging
