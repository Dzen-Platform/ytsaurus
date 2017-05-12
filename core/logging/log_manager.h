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
    void ConfigureFromEnv();

    virtual void Shutdown() override;

    int GetVersion() const;
    ELogLevel GetMinLevel(const TString& category) const;

    void Enqueue(TLogEvent&& event);

    void Reopen();

    void SetPerThreadBatchingPeriod(TDuration value);
    TDuration GetPerThreadBatchingPeriod() const;

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

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
