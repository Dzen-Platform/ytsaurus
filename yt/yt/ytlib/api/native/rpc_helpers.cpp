#include "rpc_helpers.h"
#include "config.h"

namespace NYT::NApi::NNative {

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

bool IsCachingEnabled(
    const TConnectionConfigPtr& config,
    const TMasterReadOptions& options)
{
    if (options.ReadFrom == EMasterChannelKind::LocalCache) {
        return true;
    }
    
    const auto& cache = config->MasterCache;
    if (!cache) {
        return false;
    }

    if (!cache->EnableMasterCacheDiscovery && cache->Addresses.empty()) {
        return false;
    }

    return
        options.ReadFrom == EMasterChannelKind::Cache ||
        options.ReadFrom == EMasterChannelKind::MasterCache;
}

void SetCachingHeader(
    const IClientRequestPtr& request,
    const TConnectionConfigPtr& config,
    const TMasterReadOptions& options,
    NHydra::TRevision refreshRevision)
{
    if (!IsCachingEnabled(config, options)) {
        return;
    }
    auto* cachingHeaderExt = request->Header().MutableExtension(NYTree::NProto::TCachingHeaderExt::caching_header_ext);
    cachingHeaderExt->set_success_expiration_time(ToProto<i64>(options.ExpireAfterSuccessfulUpdateTime));
    cachingHeaderExt->set_failure_expiration_time(ToProto<i64>(options.ExpireAfterFailedUpdateTime));
    if (refreshRevision != NHydra::NullRevision) {
        cachingHeaderExt->set_refresh_revision(refreshRevision);
    }
}

void SetBalancingHeader(
    const IClientRequestPtr& request,
    const TConnectionConfigPtr& config,
    const TMasterReadOptions& options)
{
    if (!IsCachingEnabled(config, options)) {
        return;
    }
    auto* balancingHeaderExt = request->Header().MutableExtension(NRpc::NProto::TBalancingExt::balancing_ext);
    balancingHeaderExt->set_enable_stickiness(true);
    balancingHeaderExt->set_sticky_group_size(std::max(config->CacheStickyGroupSizeOverride, options.CacheStickyGroupSize));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
