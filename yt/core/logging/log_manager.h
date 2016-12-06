#pragma once

#include "public.h"

#include <yt/core/misc/shutdownable.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

class TLogManager
    : public IShutdownable
{
public:
    TLogManager();
    ~TLogManager();

    static TLogManager* Get();

    static void StaticShutdown();

    void Configure(TLogConfigPtr config);

    virtual void Shutdown() override;

    int GetVersion() const;
    ELogLevel GetMinLevel(const Stroka& category) const;

    void Enqueue(TLogEvent&& event);

    void Reopen();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

void SimpleConfigureLogging(
    const char* logLevelStr,
    const char* logExcludeCategoriesStr,
    const char* logIncludeCategoriesStr);

void SimpleConfigureLoggingFromEnv();

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT

template <>
struct TSingletonTraits<NYT::NLogging::TLogManager>
{
    enum
    {
        Priority = 2048
    };
};
