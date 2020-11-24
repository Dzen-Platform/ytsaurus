#pragma once

#include "public.h"
#include "yt/server/http_proxy/private.h"

#include <yt/client/driver/driver.h>

#include <yt/core/http/http.h>

#include <yt/core/concurrency/spinlock.h>

#include <yt/yt/library/profiling/sensor.h>

#include <yt/yt/library/syncmap/map.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

using TUserCommandPair =std::pair<TString, TString>;

class TSemaphoreGuard
{
public:
    TSemaphoreGuard(TSemaphoreGuard&&) = default;
    TSemaphoreGuard& operator = (TSemaphoreGuard&&) = default;

    TSemaphoreGuard(TApi* api, const TUserCommandPair& key);
    ~TSemaphoreGuard();

private:
    struct TEmptyDeleter
    {
        void operator () (TApi* api)
        { }
    };

    std::unique_ptr<TApi, TEmptyDeleter> Api_;
    TUserCommandPair Key_;
};

////////////////////////////////////////////////////////////////////////////////

class TApi
    : public NHttp::IHttpHandler
{
public:
    explicit TApi(TBootstrap* bootstrap);

    virtual void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

    const NDriver::IDriverPtr& GetDriverV3() const;
    const NDriver::IDriverPtr& GetDriverV4() const;
    const THttpAuthenticatorPtr& GetHttpAuthenticator() const;
    const TCoordinatorPtr& GetCoordinator() const;
    const TApiConfigPtr& GetConfig() const;
    const NConcurrency::IPollerPtr& GetPoller() const;

    bool IsUserBannedInCache(const TString& user);
    void PutUserIntoBanCache(const TString& user);

    std::optional<TSemaphoreGuard> AcquireSemaphore(const TString& user, const TString& command);
    void ReleaseSemaphore(const TUserCommandPair& key);

    void IncrementProfilingCounters(
        const TString& user,
        const TString& command,
        std::optional<NHttp::EStatusCode> httpStatusCode,
        TErrorCode apiErrorCode,
        TDuration duration,
        const NNet::TNetworkAddress& clientAddress,
        i64 bytesIn,
        i64 bytesOut);

    void IncrementHttpCode(NHttp::EStatusCode httpStatusCode);

    int GetNumberOfConcurrentRequests();

private:
    const TApiConfigPtr Config_;

    const NDriver::IDriverPtr DriverV3_;
    const NDriver::IDriverPtr DriverV4_;

    const THttpAuthenticatorPtr HttpAuthenticator_;
    const TCoordinatorPtr Coordinator_;

    const NConcurrency::IPollerPtr Poller_;

    const NProfiling::TRegistry SparseProfiler_ = HttpProxyProfiler.WithSparse();

    std::vector<std::pair<NNet::TIP6Network, TString>> Networks_;
    TString DefaultNetworkName_;

    TString GetNetworkNameForAddress(const NNet::TNetworkAddress& address) const;

    YT_DECLARE_SPINLOCK(NConcurrency::TReaderWriterSpinLock, BanCacheLock_);
    THashMap<TString, TInstant> BanCache_;

    struct TProfilingCounters
    {
        std::atomic<int> LocalSemaphore{0};

        NProfiling::TGauge ConcurrencySemaphore;
        NProfiling::TCounter RequestCount;
        NProfiling::TEventTimer RequestDuration;

        NConcurrency::TSyncMap<TErrorCode, NProfiling::TCounter> ApiErrors;
    };

    std::atomic<int> GlobalSemaphore_{0};

    NConcurrency::TSyncMap<TUserCommandPair, std::unique_ptr<TProfilingCounters>> Counters_;

    NConcurrency::TSyncMap<std::pair<TString, TString>, NProfiling::TCounter> BytesIn_;
    NConcurrency::TSyncMap<std::pair<TString, TString>, NProfiling::TCounter> BytesOut_;

    NConcurrency::TSyncMap<NHttp::EStatusCode, NProfiling::TCounter> HttpCodes_;
    NConcurrency::TSyncMap<std::pair<TString, NHttp::EStatusCode>, NProfiling::TCounter> HttpCodesByUser_;
    NConcurrency::TSyncMap<std::pair<TString, NHttp::EStatusCode>, NProfiling::TCounter> HttpCodesByCommand_;

    TProfilingCounters* GetProfilingCounters(const TUserCommandPair& key);

    NProfiling::TCounter PrepareErrorCount_ = HttpProxyProfiler.Counter("/request_prepare_error_count");

    void DoIncrementHttpCode(
        THashMap<NHttp::EStatusCode, NProfiling::TCounter>* counters,
        NHttp::EStatusCode httpStatusCode,
        NProfiling::TTagIdList tags);
};

DEFINE_REFCOUNTED_TYPE(TApi)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
