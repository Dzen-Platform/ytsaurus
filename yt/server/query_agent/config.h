#pragma once

#include "public.h"

#include <yt/ytlib/query_client/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NQueryAgent {

////////////////////////////////////////////////////////////////////////////////

class TQueryAgentConfig
    : public NQueryClient::TExecutorConfig
{
public:
    int ThreadPoolSize;
    int MaxSubsplitsPerTablet;
    int MaxSubqueries;
    int MaxQueryRetries;
    int MaxBottomReaderConcurrency;

    TSlruCacheConfigPtr FunctionImplCache;

    TQueryAgentConfig()
    {
        RegisterParameter("thread_pool_size", ThreadPoolSize)
            .GreaterThan(0)
            .Default(4);
        RegisterParameter("max_subsplits_per_tablet", MaxSubsplitsPerTablet)
            .GreaterThan(0)
            .Default(64);
        RegisterParameter("max_subqueries", MaxSubqueries)
            .GreaterThan(0)
            .Default(16);
        RegisterParameter("max_query_retries", MaxQueryRetries)
            .GreaterThanOrEqual(1)
            .Default(10);
        RegisterParameter("max_bottom_reader_concurrency", MaxBottomReaderConcurrency)
            .GreaterThanOrEqual(1)
            .Default(5);

        RegisterParameter("function_impl_cache", FunctionImplCache)
            .DefaultNew();

        RegisterInitializer([&] () {
            FunctionImplCache->Capacity = 100;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TQueryAgentConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryAgent
} // namespace NYT

