#pragma once

#include "public.h"

#include "config.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/client/misc/public.h>

#include <yt/core/misc/async_expiring_cache.h>

namespace NYT::NObjectClient {

////////////////////////////////////////////////////////////////////////////////

class TObjectAttributeCache
    : public TAsyncExpiringCache<NYPath::TYPath, NYTree::TAttributeMap>
{
public:
    TObjectAttributeCache(
        TObjectAttributeCacheConfigPtr config,
        std::vector<TString> attributes,
        NApi::NNative::IClientPtr client,
        IInvokerPtr invoker,
        NLogging::TLogger logger = {},
        NProfiling::TProfiler profiler = {});

protected:
    virtual TFuture<NYTree::TAttributeMap> DoGet(const NYPath::TYPath& path) override;
    virtual TFuture<std::vector<TErrorOr<NYTree::TAttributeMap>>> DoGetMany(
        const std::vector<NYPath::TYPath>& paths) override;

private:
    const TObjectAttributeCacheConfigPtr Config_;
    const std::vector<TString> Attributes_;
    const NLogging::TLogger Logger;

    NApi::NNative::IClientPtr Client_;
    IInvokerPtr Invoker_;
};

DEFINE_REFCOUNTED_TYPE(TObjectAttributeCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectClient
