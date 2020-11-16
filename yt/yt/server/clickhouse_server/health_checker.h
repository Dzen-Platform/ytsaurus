#pragma once

#include "private.h"

#include <yt/core/actions/public.h>

#include <yt/core/concurrency/public.h>

#include <yt/core/ytree/public.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

class THealthChecker
    : public TRefCounted
{
public:
    THealthChecker(
        THealthCheckerConfigPtr config,
        TString dataBaseUser,
        DB::Context* databaseContext,
        THost* host);

    void Start();

private:
    const THealthCheckerConfigPtr Config_;
    const TString DatabaseUser_;
    DB::Context* const DatabaseContext_;
    THost* const Host_;
    NConcurrency::TActionQueuePtr ActionQueue_;
    const NConcurrency::TPeriodicExecutorPtr PeriodicExecutor_;
    std::vector<NProfiling::TGauge> QueryIndexToStatus_;

    void ExecuteQuery(const TString& query);
    void ExecuteQueries();
};

DEFINE_REFCOUNTED_TYPE(THealthChecker);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
