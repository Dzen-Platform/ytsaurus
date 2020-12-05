#pragma once

#include "public.h"

#include <yt/ytlib/query_client/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NQueryAgent {

////////////////////////////////////////////////////////////////////////////////

class TQueryAgentConfig
    : public NQueryClient::TExecutorConfig
{
public:
    int QueryThreadPoolSize;
    int LookupThreadPoolSize;
    int MaxSubsplitsPerTablet;
    int MaxSubqueries;
    int MaxQueryRetries;
    size_t DesiredUncompressedResponseBlockSize;

    TSlruCacheConfigPtr FunctionImplCache;

    TAsyncExpiringCacheConfigPtr PoolWeightCache;

    TQueryAgentConfig()
    {
        RegisterParameter("query_thread_pool_size", QueryThreadPoolSize)
            .Alias("thread_pool_size")
            .GreaterThan(0)
            .Default(4);
        RegisterParameter("lookup_thread_pool_size", LookupThreadPoolSize)
            .GreaterThan(0)
            .Default(4);
        RegisterParameter("max_subsplits_per_tablet", MaxSubsplitsPerTablet)
            .GreaterThan(0)
            .Default(4096);
        RegisterParameter("max_subqueries", MaxSubqueries)
            .GreaterThan(0)
            .Default(16);
        RegisterParameter("max_query_retries", MaxQueryRetries)
            .GreaterThanOrEqual(1)
            .Default(10);
        RegisterParameter("desired_uncompressed_response_block_size", DesiredUncompressedResponseBlockSize)
            .GreaterThan(0)
            .Default(16_MB);

        RegisterParameter("function_impl_cache", FunctionImplCache)
            .DefaultNew();

        RegisterParameter("pool_weight_cache", PoolWeightCache)
            .DefaultNew();

        RegisterPreprocessor([&] () {
            FunctionImplCache->Capacity = 100;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TQueryAgentConfig)

////////////////////////////////////////////////////////////////////////////////

class TQueryAgentDynamicConfig
    : public NYTree::TYsonSerializable
{
public:
    std::optional<int> QueryThreadPoolSize;
    std::optional<int> LookupThreadPoolSize;

    TQueryAgentDynamicConfig()
    {
        RegisterParameter("query_thread_pool_size", QueryThreadPoolSize)
            .Alias("thread_pool_size")
            .GreaterThan(0)
            .Optional();
        RegisterParameter("lookup_thread_pool_size", LookupThreadPoolSize)
            .GreaterThan(0)
            .Optional();
    }
};

DEFINE_REFCOUNTED_TYPE(TQueryAgentDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryAgent

