#include "helpers.h"
#include "config.h"

#include <yt/yt/ytlib/chunk_client/dispatcher.h>

#include <yt/yt/core/ytalloc/bindings.h>

#include <yt/yt/core/misc/lazy_ptr.h>
#include <yt/yt/core/misc/ref_counted_tracker.h>
#include <yt/yt/core/misc/ref_counted_tracker_profiler.h>

#include <yt/yt/core/bus/tcp/dispatcher.h>

#include <yt/yt/library/tracing/jaeger/tracer.h>

#include <yt/yt/library/profiling/perf/counters.h>

#include <yt/yt/library/profiling/resource_tracker/resource_tracker.h>

#include <yt/yt/core/profiling/profile_manager.h>

#include <yt/yt/core/logging/log_manager.h>

#include <yt/yt/core/concurrency/execution_stack.h>
#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/private.h>

#include <tcmalloc/malloc_extension.h>

#include <yt/yt/core/net/address.h>
#include <yt/yt/core/net/local_address.h>

#include <yt/yt/core/rpc/dispatcher.h>

#include <yt/yt/core/service_discovery/yp/service_discovery.h>

#include <yt/yt/core/threading/spin_wait_slow_path_logger.h>

#include <library/cpp/yt/threading/spin_wait_hook.h>

#include <util/string/split.h>

namespace NYT {

using namespace NConcurrency;
using namespace NThreading;

////////////////////////////////////////////////////////////////////////////////

void ConfigureTCMalloc(const TTCMallocConfigPtr& config)
{
    tcmalloc::MallocExtension::SetBackgroundReleaseRate(
        tcmalloc::MallocExtension::BytesPerSecond{static_cast<size_t>(config->BackgroundReleaseRate)});

    tcmalloc::MallocExtension::SetMaxPerCpuCacheSize(config->MaxPerCpuCacheSize);
}

template <class TConfig>
void ConfigureSingletonsImpl(const TConfig& config)
{
    SetSpinWaitSlowPathLoggingThreshold(config->SpinWaitSlowPathLoggingThreshold);

    if (!NYTAlloc::ConfigureFromEnv()) {
        NYTAlloc::Configure(config->YTAlloc);
    }

    for (const auto& [kind, size] : config->FiberStackPoolSizes) {
        NConcurrency::SetFiberStackPoolSize(ParseEnum<NConcurrency::EExecutionStackKind>(kind), size);
    }

    NLogging::TLogManager::Get()->EnableReopenOnSighup();
    if (!NLogging::TLogManager::Get()->IsConfiguredFromEnv()) {
        NLogging::TLogManager::Get()->Configure(config->Logging);
    }

    NNet::TAddressResolver::Get()->Configure(config->AddressResolver);
    // By default, server component must have reasonable fqdn.
    // Failure to do so may result in issues like YT-4561.
    NNet::TAddressResolver::Get()->EnsureLocalHostName();

    NBus::TTcpDispatcher::Get()->Configure(config->TcpDispatcher);

    NRpc::TDispatcher::Get()->Configure(config->RpcDispatcher);

    NRpc::TDispatcher::Get()->SetServiceDiscovery(
        NServiceDiscovery::NYP::CreateServiceDiscovery(config->YPServiceDiscovery));

    NChunkClient::TDispatcher::Get()->Configure(config->ChunkClientDispatcher);

    NTracing::SetGlobalTracer(New<NTracing::TJaegerTracer>(config->Jaeger));

    NProfiling::TProfileManager::Get()->Configure(config->ProfileManager);
    NProfiling::TProfileManager::Get()->Start();

    NProfiling::EnablePerfCounters();

    if (auto tracingConfig = config->Rpc->Tracing) {
        NTracing::SetTracingConfig(tracingConfig);
    }

    ConfigureTCMalloc(config->TCMalloc);

    if (config->EnableRefCountedTrackerProfiling) {
        EnableRefCountedTrackerProfiling();
    }

    if (config->EnableResourceTracker) {
        NProfiling::EnableResourceTracker();
    }
}

void ConfigureSingletons(const TSingletonsConfigPtr& config)
{
    ConfigureSingletonsImpl(config);
}

template <class TStaticConfig, class TDynamicConfig>
void ReconfigureSingletonsImpl(const TStaticConfig& config, const TDynamicConfig& dynamicConfig)
{
    SetSpinWaitSlowPathLoggingThreshold(dynamicConfig->SpinWaitSlowPathLoggingThreshold.value_or(config->SpinWaitSlowPathLoggingThreshold));

    if (!NYTAlloc::IsConfiguredFromEnv()) {
        NYTAlloc::Configure(dynamicConfig->YTAlloc ? dynamicConfig->YTAlloc : config->YTAlloc);
    }

    if (!NLogging::TLogManager::Get()->IsConfiguredFromEnv()) {
        NLogging::TLogManager::Get()->Configure(
            config->Logging->ApplyDynamic(dynamicConfig->Logging),
            /*sync*/ false);
    }

    auto tracer = NTracing::GetGlobalTracer();
    if (auto jaeger = DynamicPointerCast<NTracing::TJaegerTracer>(tracer); jaeger) {
        jaeger->Configure(config->Jaeger->ApplyDynamic(dynamicConfig->Jaeger));
    }

    NBus::TTcpDispatcher::Get()->Configure(config->TcpDispatcher->ApplyDynamic(dynamicConfig->TcpDispatcher));

    NRpc::TDispatcher::Get()->Configure(config->RpcDispatcher->ApplyDynamic(dynamicConfig->RpcDispatcher));

    NChunkClient::TDispatcher::Get()->Configure(config->ChunkClientDispatcher->ApplyDynamic(dynamicConfig->ChunkClientDispatcher));

    if (dynamicConfig->Rpc->Tracing) {
        NTracing::SetTracingConfig(dynamicConfig->Rpc->Tracing);
    } else if (config->Rpc->Tracing) {
        NTracing::SetTracingConfig(config->Rpc->Tracing);
    }

    if (dynamicConfig->TCMalloc) {
        ConfigureTCMalloc(dynamicConfig->TCMalloc);
    } else if (config->TCMalloc) {
        ConfigureTCMalloc(config->TCMalloc);
    }
}

void ReconfigureSingletons(const TSingletonsConfigPtr& config, const TSingletonsDynamicConfigPtr& dynamicConfig)
{
    ReconfigureSingletonsImpl(config, dynamicConfig);
}

template <class TConfig>
void StartDiagnosticDumpImpl(const TConfig& config)
{
    static NLogging::TLogger Logger("DiagDump");

    auto logDumpString = [&] (TStringBuf banner, const TString& str) {
        for (const auto& line : StringSplitter(str).Split('\n')) {
            YT_LOG_DEBUG("%v %v", banner, line.Token());
        }
    };

    if (config->YTAllocDumpPeriod) {
        static const TLazyIntrusivePtr<TPeriodicExecutor> Executor(BIND([&] {
            return New<TPeriodicExecutor>(
                NRpc::TDispatcher::Get()->GetHeavyInvoker(),
                BIND([&] {
                    logDumpString("YTAlloc", NYTAlloc::FormatAllocationCounters());
                }));
        }));
        Executor->SetPeriod(config->YTAllocDumpPeriod);
        Executor->Start();
    }

    if (config->RefCountedTrackerDumpPeriod) {
        static const TLazyIntrusivePtr<TPeriodicExecutor> Executor(BIND([&] {
            return New<TPeriodicExecutor>(
                NRpc::TDispatcher::Get()->GetHeavyInvoker(),
                BIND([&] {
                    logDumpString("RCT", TRefCountedTracker::Get()->GetDebugInfo());
                }));
        }));
        Executor->SetPeriod(config->RefCountedTrackerDumpPeriod);
        Executor->Start();
    }
}

void StartDiagnosticDump(const TDiagnosticDumpConfigPtr& config)
{
    StartDiagnosticDumpImpl(config);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
