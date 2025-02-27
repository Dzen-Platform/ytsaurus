#include <yt/yt/server/node/tablet_node/overload_controller.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/new_fair_share_thread_pool.h>
#include <yt/yt/core/concurrency/two_level_fair_share_thread_pool.h>

#include <yt/yt/core/test_framework/framework.h>

namespace NYT::NTabletNode {
namespace {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TMockInvoker
    : public IInvoker
{
public:
    TWaitTimeObserver WaitTimeObserver;

    void Invoke(TClosure /*callback*/) override
    { }

    void Invoke(TMutableRange<TClosure> /*callbacks*/) override
    { }

    bool CheckAffinity(const IInvokerPtr& /*invoker*/) const override
    {
        return false;
    }

    bool IsSerialized() const override
    {
        return true;
    }

    NThreading::TThreadId GetThreadId() const override
    {
        return {};
    }

    void RegisterWaitTimeObserver(TWaitTimeObserver waitTimeObserver) override
    {
        WaitTimeObserver = waitTimeObserver;
    }
};

////////////////////////////////////////////////////////////////////////////////

using TMethodInfo = std::vector<std::pair<TString, TString>>;

static const TDuration MeanWaitTimeThreshold = TDuration::MilliSeconds(20);

TOverloadControllerConfigPtr CreateConfig(const THashMap<TString, TMethodInfo>& schema)
{
    auto config = New<TOverloadControllerConfig>();
    config->Enabled = true;

    for (const auto& [trackerName, methods] : schema) {
        auto trackerConfig = New<TOverloadTrackerConfig>();

        for (const auto& [service, method] : methods) {
            auto serviceMethod = New<TServiceMethod>();
            serviceMethod->Service = service;
            serviceMethod->Method = method;

            trackerConfig->MethodsToThrottle.push_back(std::move(serviceMethod));
            trackerConfig->MeanWaitTimeThreshold = MeanWaitTimeThreshold;
        }

        config->Trackers[trackerName] = trackerConfig;
    }

    return config;
}

////////////////////////////////////////////////////////////////////////////////

TEST(TOverloadControllerTest, TestOverloadsRequests)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();
    auto mockInvoker2 = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);
    controller->TrackInvoker("Mock2", mockInvoker2);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod"}}},
        {"Mock2", {{"MockService", "MockMethod2"}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);
    controller->Reconfigure(config);

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
    }

    // Check overload incoming requests
    int remainsCount = 1000;
    while (remainsCount > 0) {
        EXPECT_FALSE(controller->GetOverloadStatus({}, "MockService", "MockMethod2", {}).Overloaded);

        auto status = controller->GetOverloadStatus({}, "MockService", "MockMethod", {});
        if (status.Overloaded) {
            --remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }

    // Check recovering even if no calls
    while (remainsCount < 1000) {
        auto status = controller->GetOverloadStatus({}, "MockService", "MockMethod", {});
        if (!status.Overloaded) {
            ++remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(1));
        }
    }
}

TEST(TOverloadControllerTest, TestNoOverloads)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod"}}}
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);

    controller->Reconfigure(config);

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold / 2);
    }

    for (int i = 0; i < 10000; ++i) {
        EXPECT_FALSE(controller->GetOverloadStatus({}, "MockService", "MockMethod", {}).Overloaded);
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold / 2);

        Sleep(TDuration::MicroSeconds(10));
    }
}

TEST(TOverloadControllerTest, TestTwoInvokersSameMethod)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();
    auto mockInvoker2 = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);
    controller->TrackInvoker("Mock2", mockInvoker2);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod"}}},
        {"Mock2", {{"MockService", "MockMethod"}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);

    controller->Reconfigure(config);

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
        mockInvoker2->WaitTimeObserver(MeanWaitTimeThreshold / 2);
    }

    // Check overloading incoming requests
    int remainsCount = 1000;
    while (remainsCount > 0) {
        auto status = controller->GetOverloadStatus({}, "MockService", "MockMethod", {});
        if (status.Overloaded) {
            --remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }

    // Check recovering even if no calls
    while (remainsCount < 1000) {
        auto status = controller->GetOverloadStatus({}, "MockService", "MockMethod", {});
        if (!status.Overloaded) {
            ++remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(1));
        }
    }
}

TEST(TOverloadControllerTest, TestThrottlingAndSkips)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();
    auto mockInvoker2 = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod"}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(200);
    config->ThrottlingStepTime = TDuration::MilliSeconds(12);
    config->MaxThrottlingTime = TDuration::MilliSeconds(127);

    controller->Reconfigure(config);

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
    }

    // Check overload incoming requests
    int remainsCount = 1000;
    while (remainsCount > 0) {
        auto status = controller->GetOverloadStatus({}, "MockService", "MockMethod", {});
        if (status.Overloaded) {
            break;
        } else {
            Sleep(TDuration::MilliSeconds(10));
        }
    }

    {
        auto status = controller->GetOverloadStatus({}, "MockService", "MockMethod", {});
        EXPECT_TRUE(status.Overloaded);
        EXPECT_FALSE(status.SkipCall);
        EXPECT_EQ(status.ThrottleTime, config->ThrottlingStepTime);
    }

    {
        auto status = controller->GetOverloadStatus(config->MaxThrottlingTime / 2, "MockService", "MockMethod", {});
        EXPECT_TRUE(status.Overloaded);
        EXPECT_FALSE(status.SkipCall);
        EXPECT_EQ(status.ThrottleTime, config->ThrottlingStepTime);
    }

    {
        auto status = controller->GetOverloadStatus(
            config->MaxThrottlingTime - config->ThrottlingStepTime,
            "MockService",
            "MockMethod",
            {});

        EXPECT_TRUE(status.Overloaded);
        EXPECT_TRUE(status.SkipCall);
        EXPECT_EQ(status.ThrottleTime, config->ThrottlingStepTime);
    }

    {
        auto status = controller->GetOverloadStatus(config->MaxThrottlingTime * 2, "MockService", "MockMethod", {});
        EXPECT_TRUE(status.Overloaded);
        EXPECT_TRUE(status.SkipCall);
        EXPECT_EQ(status.ThrottleTime, config->ThrottlingStepTime);
    }

    {
        auto status = controller->GetOverloadStatus(
            config->MaxThrottlingTime * 2,
            "MockService",
            "MockMethod",
            config->MaxThrottlingTime * 4);

        EXPECT_TRUE(status.Overloaded);
        EXPECT_TRUE(status.SkipCall);
        EXPECT_EQ(status.ThrottleTime, config->ThrottlingStepTime);
    }

    {
        auto status = controller->GetOverloadStatus(
            config->MaxThrottlingTime / 2,
            "MockService",
            "MockMethod",
            config->MaxThrottlingTime / 4);

        EXPECT_TRUE(status.Overloaded);
        EXPECT_TRUE(status.SkipCall);
        EXPECT_EQ(status.ThrottleTime, config->ThrottlingStepTime);
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class TExecutorPtr>
void ExecuteWaitTimeTest(const TExecutorPtr& executor, const IInvokerPtr& invoker)
{
    static constexpr int DesiredActionsCount = 27;

    TDuration totalWaitTime;
    int actionsCount = 0;

    executor->RegisterWaitTimeObserver([&] (TDuration waitTime) {
        totalWaitTime += waitTime;
        ++actionsCount;
    });

    std::vector<TFuture<void>> futures;
    for (int i = 0; i < DesiredActionsCount; ++i) {
        auto future = BIND([] {
            Sleep(TDuration::MilliSeconds(1));
        }).AsyncVia(invoker)
        .Run();

        futures.push_back(std::move(future));
    }

    WaitFor(AllSucceeded(std::move(futures)))
        .ThrowOnError();

    EXPECT_EQ(DesiredActionsCount, actionsCount);
    EXPECT_GE(totalWaitTime, TDuration::MilliSeconds(DesiredActionsCount - 1));
}

TEST(TOverloadControllerTest, WaitTimeObserver)
{
    {
        auto actionQueue = New<TActionQueue>("TestActionQueue");
        ExecuteWaitTimeTest(actionQueue->GetInvoker(), actionQueue->GetInvoker());
    }

    {
        auto fshThreadPool = CreateTwoLevelFairShareThreadPool(1, "TestFsh");
        ExecuteWaitTimeTest(fshThreadPool, fshThreadPool->GetInvoker("test-pool", "fsh-tag"));
    }

    {
        auto fshThreadPool = CreateNewTwoLevelFairShareThreadPool(1, "TestNewFsh");
        ExecuteWaitTimeTest(fshThreadPool, fshThreadPool->GetInvoker("test-pool", "fsh-tag"));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTabletNode
