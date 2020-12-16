#include <random>

#include <unistd.h>

#include <yt/core/concurrency/poller.h>
#include <yt/core/concurrency/thread_pool_poller.h>
#include <yt/core/concurrency/action_queue.h>

#include <yt/core/profiling/resource_tracker.h>

#include <yt/core/http/server.h>

#include <yt/core/ytalloc/bindings.h>

#include <yt/core/misc/ref_counted_tracker_profiler.h>

#include <yt/core/misc/shutdown.h>

#include <yt/yt/library/profiling/sensor.h>
#include <yt/yt/library/profiling/solomon/exporter.h>

#include <util/stream/output.h>
#include <util/system/compiler.h>
#include <util/generic/yexception.h>
#include <util/string/cast.h>

using namespace NYT;
using namespace NYT::NHttp;
using namespace NYT::NConcurrency;
using namespace NYT::NProfiling;

int main(int argc, char* argv[])
{
    try {
        if (argc != 2 && argc != 3) {
            throw yexception() << "usage: " << argv[0] << " PORT [--fast]";
        }

        auto port = FromString<int>(argv[1]);
        auto fast = TString{"--fast"} == TString{argv[2]};
        auto poller = CreateThreadPoolPoller(1, "Example");
        auto server = CreateServer(port, poller);
        auto actionQueue = New<TActionQueue>("Control");

        auto internalShardConfig = New<TShardConfig>();
        internalShardConfig->Filter = {"yt/solomon"};

        auto ytallocShardConfig = New<TShardConfig>();
        ytallocShardConfig->Filter = {"yt/ytalloc"};

        auto defaultShardConfig = New<TShardConfig>();
        defaultShardConfig->Filter = {""};

        auto config = New<TSolomonExporterConfig>();
        config->Shards = {
            {"internal", internalShardConfig},
            {"ytalloc", ytallocShardConfig},
            {"default", defaultShardConfig},
        };

        if (fast) {
            config->GridStep = TDuration::Seconds(2);
        }

        // Deprecated option. Enabled for testing.
        config->EnableCoreProfilingCompatibility = true;

        auto exporter = New<TSolomonExporter>(config, actionQueue->GetInvoker());
        exporter->Register("/solomon", server);
        exporter->Start();

        server->Start();

        NYTAlloc::EnableYTProfiling();
        EnableRefCountedTrackerProfiling();

        TRegistry r{"/my_loop"};

        auto iterationCount = r.WithTag("thread", "main").Counter("/iteration_count");
        auto randomNumber = r.WithTag("thread", "main").Gauge("/random_number");

        auto invalidCounter = r.Counter("/invalid");
        auto invalidGauge = r.Gauge("/invalid");

        auto sparseCounter = r.WithSparse().Counter("/sparse_count");
        
        auto poolUsage = r.WithTag("pool", "prime").WithGlobal().Gauge("/cpu");
        poolUsage.Update(3000.0);

        std::default_random_engine rng;
        double value = 0.0;

        for (i64 i = 0; true; ++i) {
            YT_PROFILE_TIMING("/loop_start") {
                iterationCount.Increment();
                randomNumber.Update(value);
            }
            value += std::uniform_real_distribution<double>(-1, 1)(rng);

            YT_PROFILE_TIMING("/busy_wait") {
                // Busy wait to demonstrate CPU tracker.
                auto endBusyTime = TInstant::Now() + TDuration::MilliSeconds(10);
                while (TInstant::Now() < endBusyTime)
                { }
            }

            if (i % 18000 == 0) {
                sparseCounter.Increment();
            }
        }
    } catch (const std::exception& ex) {
        Cerr << ex.what() << Endl;
        _exit(1);
    }

    return 0;
}
