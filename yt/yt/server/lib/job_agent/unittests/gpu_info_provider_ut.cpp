#include <yt/yt/server/lib/job_agent/config.h>
#include <yt/yt/server/lib/job_agent/gpu_info_provider.h>
#include <yt/yt/server/lib/job_agent/nvgpu_manager.h>

#include <yt/yt/core/rpc/server.h>
#include <yt/yt/core/rpc/service_detail.h>

#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/rpc/grpc/config.h>
#include <yt/yt/core/rpc/grpc/channel.h>
#include <yt/yt/core/rpc/grpc/server.h>
#include <yt/yt/core/rpc/grpc/proto/grpc.pb.h>

#include <yt/yt/core/yson/string.h>

#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/ytree/yson_serializable.h>

#include <library/cpp/testing/common/network.h>

namespace NYT::NJobAgent {
namespace {

using namespace NYson;
using namespace NYTree;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TMockNvGpuManagerService
    : public NRpc::TServiceBase
{
public:
    TMockNvGpuManagerService(
        IInvokerPtr invoker)
    : TServiceBase(
        invoker,
        TNvGpuManagerService::GetDescriptor(),
        NLogging::TLogger("TMockNvGpuManagerService"),
        NRpc::NullRealmId)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ListDevices));
    }

    DECLARE_RPC_SERVICE_METHOD(NYT::NJobAgent, ListDevices)
    {
        {
            auto* dev = response->add_devices();
            auto* spec = dev->mutable_spec()->mutable_nvidia();
            spec->set_uuid("dev1");
            spec->set_power(123);
            spec->set_memory_size_mb(123);
            auto* status = dev->mutable_status()->mutable_nvidia();
            status->set_gpu_utilization(50);
            status->set_memory_utilization(25);
            status->set_memory_used_mb(100);
            status->set_power(100);
        }

        response->add_devices();

        {
            auto* dev = response->add_devices();
            auto* spec = dev->mutable_spec()->mutable_nvidia();
            spec->set_uuid("dev2");
            spec->set_power(234);
            spec->set_memory_size_mb(234);
            auto* status = dev->mutable_status()->mutable_nvidia();
            status->set_gpu_utilization(75);
            status->set_memory_utilization(50);
            status->set_memory_used_mb(200);
            status->set_power(200);
        }

        context->Reply();
    }
};

class TTestNvManagerGpuInfoProvider
    : public ::testing::Test
{
public:
    void SetUp() final
    {
        Port_ = NTesting::GetFreePort();
        Address_ = Format("localhost:%v", Port_);

        Server_ = CreateServer(Port_);
        WorkerPool_ = New<NConcurrency::TThreadPool>(4, "Worker");
        NvGpuManagerService_ = New<TMockNvGpuManagerService>(WorkerPool_->GetInvoker());
        Server_->RegisterService(NvGpuManagerService_);
        Server_->Start();
    }

    void TearDown() final
    {
        Server_->Stop().Get().ThrowOnError();
        Server_.Reset();
    }

    IServerPtr CreateServer(ui16 port)
    {
        auto serverAddressConfig = New<NGrpc::TServerAddressConfig>();
        auto address = Format("localhost:%v", port);
        serverAddressConfig->Address = address;
        auto serverConfig = New<NGrpc::TServerConfig>();
        serverConfig->Addresses.push_back(serverAddressConfig);
        return NGrpc::CreateServer(serverConfig);
    }

protected:
    NTesting::TPortHolder Port_;
    TString Address_;

    NConcurrency::TThreadPoolPtr WorkerPool_;
    IServicePtr NvGpuManagerService_;
    IServerPtr Server_;
};

TEST_F(TTestNvManagerGpuInfoProvider, Simple)
{
    auto provider = New<TNvManagerGpuInfoProvider>(Address_);
    auto gpuInfos = provider->GetGpuInfos(TDuration::Max());

    {
        const auto& gpuInfo = gpuInfos[0];
        EXPECT_EQ(gpuInfo.Index, 0);
        EXPECT_EQ(gpuInfo.UtilizationGpuRate, 0.50);
        EXPECT_EQ(gpuInfo.UtilizationMemoryRate, 0.25);
        EXPECT_EQ(gpuInfo.MemoryUsed, static_cast<i64>(100_MB));
        EXPECT_EQ(gpuInfo.MemoryTotal, static_cast<i64>(123_MB));
        EXPECT_EQ(gpuInfo.PowerDraw, 100);
        EXPECT_EQ(gpuInfo.PowerLimit, 123);
        EXPECT_EQ(gpuInfo.Name, "dev1");
    }

    {
        const auto& gpuInfo = gpuInfos[1];
        EXPECT_EQ(gpuInfo.Index, 2);
        EXPECT_EQ(gpuInfo.UtilizationGpuRate, 0.75);
        EXPECT_EQ(gpuInfo.UtilizationMemoryRate, 0.50);
        EXPECT_EQ(gpuInfo.MemoryUsed, static_cast<i64>(200_MB));
        EXPECT_EQ(gpuInfo.MemoryTotal, static_cast<i64>(234_MB));
        EXPECT_EQ(gpuInfo.PowerDraw, 200);
        EXPECT_EQ(gpuInfo.PowerLimit, 234);
        EXPECT_EQ(gpuInfo.Name, "dev2");
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTabletBalancer
