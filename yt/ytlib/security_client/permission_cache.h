#pragma once

#include "public.h"
#include "config.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/core/misc/async_expiring_cache.h>

namespace NYT::NSecurityClient {

////////////////////////////////////////////////////////////////////////////////

struct TPermissionKey
{
    TString Object;
    TString User;
    NYTree::EPermission Permission;
    std::optional<std::vector<TString>> Columns;

    // Hasher.
    operator size_t() const;

    // Comparer.
    bool operator == (const TPermissionKey& other) const;

    // Formatter.
    friend TString ToString(const TPermissionKey& key);
};

////////////////////////////////////////////////////////////////////////////////

class TPermissionCache
    : public TAsyncExpiringCache<TPermissionKey, void>
{
public:
    TPermissionCache(
        TPermissionCacheConfigPtr config,
        NApi::NNative::IConnectionPtr connection,
        NProfiling::TProfiler profiler = {});

private:
    const TPermissionCacheConfigPtr Config_;
    const TWeakPtr<NApi::NNative::IConnection> Connection_;

    virtual TFuture<void> DoGet(
        const TPermissionKey& key,
        bool isPeriodicUpdate) override;
    virtual TFuture<std::vector<TError>> DoGetMany(
        const std::vector<TPermissionKey>& keys,
        bool isPeriodicUpdate) override;
};

DEFINE_REFCOUNTED_TYPE(TPermissionCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityClient
