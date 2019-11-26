#include "api.h"

#include "bootstrap.h"
#include "config.h"
#include "context.h"
#include "private.h"

#include <yt/core/http/helpers.h>

#include <yt/core/profiling/profile_manager.h>

namespace NYT::NHttpProxy {

using namespace NConcurrency;
using namespace NHttp;
using namespace NProfiling;

static const auto& Logger = HttpProxyLogger;

////////////////////////////////////////////////////////////////////////////////

TSemaphoreGuard::TSemaphoreGuard(TApi* api, const TUserCommandPair& key)
    : Api_(api)
    , Key_(key)
{ }

TSemaphoreGuard::~TSemaphoreGuard()
{
    if (Api_) {
        Api_->ReleaseSemaphore(Key_);
    }
}

////////////////////////////////////////////////////////////////////////////////

TApi::TApi(TBootstrap* bootstrap)
    : Config_(bootstrap->GetConfig()->Api)
    , DriverV3_(bootstrap->GetDriverV3())
    , DriverV4_(bootstrap->GetDriverV4())
    , HttpAuthenticator_(bootstrap->GetHttpAuthenticator())
    , Coordinator_(bootstrap->GetCoordinator())
{ }

const NDriver::IDriverPtr& TApi::GetDriverV3() const
{
    return DriverV3_;
}

const NDriver::IDriverPtr& TApi::GetDriverV4() const
{
    return DriverV4_;
}

const THttpAuthenticatorPtr& TApi::GetHttpAuthenticator() const
{
    return HttpAuthenticator_;
}

const TCoordinatorPtr& TApi::GetCoordinator() const
{
    return Coordinator_;
}

const TApiConfigPtr& TApi::GetConfig() const
{
    return Config_;
}

bool TApi::IsUserBannedInCache(const TString& user)
{
    auto now = TInstant::Now();
    TReaderGuard guard(BanCacheLock_);
    auto it = BanCache_.find(user);
    if (it != BanCache_.end()) {
        return now < it->second;
    }

    return false;
}

void TApi::PutUserIntoBanCache(const TString& user)
{
    TWriterGuard guard(BanCacheLock_);
    BanCache_[user] = TInstant::Now() + Config_->BanCacheExpirationTime;
}

int TApi::GetNumberOfConcurrentRequests()
{
    return GlobalSemaphore_.load();
}

std::optional<TSemaphoreGuard> TApi::AcquireSemaphore(const TString& user, const TString& command)
{
    auto value = GlobalSemaphore_.load();
    do {
        if (value >= Config_->ConcurrencyLimit * 2) {
            return {};
        }
    } while (!GlobalSemaphore_.compare_exchange_weak(value, value + 1));

    auto key = std::make_pair(user, command);
    auto counters = GetProfilingCounters(key);
    if (counters->LocalSemaphore >= Config_->ConcurrencyLimit) {
        GlobalSemaphore_.fetch_add(-1);
        return {};
    }

    counters->LocalSemaphore.fetch_add(1);
    HttpProxyProfiler.Increment(counters->ConcurrencySemaphore);

    return TSemaphoreGuard(this, key);
}

void TApi::ReleaseSemaphore(const TUserCommandPair& key)
{
    auto counters = GetProfilingCounters(key);
    GlobalSemaphore_.fetch_add(-1);
    counters->LocalSemaphore.fetch_add(-1);
    HttpProxyProfiler.Increment(counters->ConcurrencySemaphore, -1);
}

TApi::TProfilingCounters* TApi::GetProfilingCounters(const TUserCommandPair& key)
{
    {
        TReaderGuard guard(CountersLock_);
        auto counter = Counters_.find(key);
        if (counter != Counters_.end()) {
            return counter->second.get();
        }
    }

    auto userTag = TProfileManager::Get()->RegisterTag("user", key.first);
    auto commandTag = TProfileManager::Get()->RegisterTag("command", key.second);

    auto counters = std::make_unique<TProfilingCounters>();
    counters->Tags = { userTag, commandTag };
    counters->UserTag = { userTag };
    counters->CommandTag = { commandTag };

    counters->ConcurrencySemaphore = { "/concurrency_semaphore", counters->Tags };
    counters->RequestCount = { "/request_count", counters->Tags };
    counters->BytesIn = { "/bytes_in", counters->Tags };
    counters->BytesOut = { "/bytes_out", counters->Tags };
    counters->RequestDuration = { "/request_duration", counters->Tags };

    TWriterGuard guard(CountersLock_);
    auto result = Counters_.emplace(key, std::move(counters));
    return result.first->second.get();
}

void TApi::IncrementHttpCode(EStatusCode httpStatusCode)
{
    auto guard = Guard(HttpCodesLock_);
    DoIncrementHttpCode(&HttpCodes_, httpStatusCode, {});
}

void TApi::DoIncrementHttpCode(
    THashMap<NHttp::EStatusCode, TMonotonicCounter>* counters,
    EStatusCode httpStatusCode,
    TTagIdList tags)
{
    auto it = counters->find(httpStatusCode);

    if (it == counters->end()) {
        tags.push_back(TProfileManager::Get()->RegisterTag("http_code", static_cast<int>(httpStatusCode)));

        it = counters->emplace(
            httpStatusCode,
            TMonotonicCounter{"/http_code_count", tags}).first;
    }

    HttpProxyProfiler.Increment(it->second);
}

void TApi::IncrementProfilingCounters(
    const TString& user,
    const TString& command,
    std::optional<EStatusCode> httpStatusCode,
    TErrorCode apiErrorCode,
    TDuration duration,
    i64 bytesIn,
    i64 bytesOut)
{
    auto counters = GetProfilingCounters({user, command});

    HttpProxyProfiler.Increment(counters->RequestCount);
    HttpProxyProfiler.Increment(counters->BytesIn, bytesIn);
    HttpProxyProfiler.Increment(counters->BytesOut, bytesOut);

    HttpProxyProfiler.Update(counters->RequestDuration, duration.MilliSeconds());

    auto guard = Guard(counters->Lock);
    if (httpStatusCode) {
        DoIncrementHttpCode(&counters->HttpCodes, *httpStatusCode, counters->CommandTag);
        DoIncrementHttpCode(&counters->HttpCodes, *httpStatusCode, counters->UserTag);
    }

    if (apiErrorCode) {
        auto it = counters->ApiErrors.find(apiErrorCode);

        if (it == counters->ApiErrors.end()) {
            auto tags = counters->Tags;
            tags.push_back(TProfileManager::Get()->RegisterTag("error_code", static_cast<int>(apiErrorCode)));

            it = counters->ApiErrors.emplace(
                apiErrorCode,
                TMonotonicCounter{"/api_error_count", tags}).first;
        }

        HttpProxyProfiler.Increment(it->second);
    }
}

void TApi::HandleRequest(
    const IRequestPtr& req,
    const IResponseWriterPtr& rsp)
{
    if (MaybeHandleCors(req, rsp, Config_->DisableCorsCheck)) {
        return;
    }

    auto context = New<TContext>(MakeStrong(this), req, rsp);
    try {
        if (!context->TryPrepare()) {
            HttpProxyProfiler.Increment(PrepareErrorCount_);
            auto statusCode = rsp->GetStatus();
            if (statusCode) {
                IncrementHttpCode(*statusCode);
            }
            return;
        }

        context->FinishPrepare();
        context->Run();
    } catch (const std::exception& ex) {
        context->SetError(TError(ex));
        YT_LOG_ERROR(ex, "Command failed");
    }

    context->Finalize();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
