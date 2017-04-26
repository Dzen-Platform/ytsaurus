#include "snapshot_downloader.h"

#include <yt/server/scheduler/config.h>

#include <yt/server/cell_scheduler/bootstrap.h>

#include <yt/ytlib/api/native_client.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/core/concurrency/async_stream.h>

namespace NYT {
namespace NControllerAgent {

using namespace NApi;
using namespace NConcurrency;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

TSnapshotDownloader::TSnapshotDownloader(
    TSchedulerConfigPtr config,
    NCellScheduler::TBootstrap* bootstrap,
    const TOperationId& operationId)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , OperationId_(operationId)
    , Logger(NLogging::TLogger(MasterConnectorLogger)
        .AddTag("OperationId: %v", operationId))
{
    YCHECK(bootstrap);
}

TSharedRef TSnapshotDownloader::Run()
{
    LOG_INFO("Starting downloading snapshot");

    auto client = Bootstrap_->GetMasterClient();

    auto snapshotPath = GetSnapshotPath(OperationId_);

    IAsyncZeroCopyInputStreamPtr reader;
    {
        TFileReaderOptions options;
        options.Config = Config_->SnapshotReader;
        reader = WaitFor(client->CreateFileReader(snapshotPath, options))
            .ValueOrThrow();
    }

    LOG_INFO("Snapshot reader opened");

    std::vector<TSharedRef> blocks;
    while (true) {
        auto blockOrError = WaitFor(reader->Read());
        auto block = blockOrError.ValueOrThrow();
        if (!block)
            break;
        blocks.push_back(block);
    }

    LOG_INFO("Snapshot downloaded successfully");

    struct TSnapshotDataTag { };
    return MergeRefsToRef<TSnapshotDataTag>(blocks);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT
