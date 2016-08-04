#include "snapshot_builder.h"
#include "private.h"
#include "config.h"
#include "helpers.h"
#include "scheduler.h"
#include "serialize.h"

#include <yt/ytlib/api/file_writer.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/misc/fs.h>
#include <yt/core/misc/proc.h>

#include <yt/core/pipes/async_reader.h>
#include <yt/core/pipes/async_writer.h>
#include <yt/core/pipes/pipe.h>

#include <thread>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NObjectClient;
using namespace NConcurrency;
using namespace NApi;
using namespace NPipes;

////////////////////////////////////////////////////////////////////////////////

static const size_t PipeWriteBufferSize = (size_t) 1024 * 1024;
static const size_t RemoteWriteBufferSize = (size_t) 1024 * 1024;

////////////////////////////////////////////////////////////////////////////////

struct TSnapshotJob
{
    TOperationPtr Operation;
    std::unique_ptr<TFile> OutputFile;
};

////////////////////////////////////////////////////////////////////////////////

TSnapshotBuilder::TSnapshotBuilder(
    TSchedulerConfigPtr config,
    TSchedulerPtr scheduler,
    IClientPtr client)
    : Config_(config)
    , Scheduler_(scheduler)
    , Client_(client)
    , Profiler(SchedulerProfiler.GetPathPrefix() + "/snapshot")
{
    YCHECK(Config_);
    YCHECK(Scheduler_);
    YCHECK(Client_);

    Logger = SchedulerLogger;
}

TFuture<void> TSnapshotBuilder::Run()
{
    LOG_INFO("Snapshot builder started");

    std::vector<TFuture<void>> operationSuspendFutures;
    std::vector<TOperationId> operationIds;

    // Capture everything needed in Build.
    for (auto operation : Scheduler_->GetOperations()) {
        if (operation->GetState() != EOperationState::Running)
            continue;

        TJob job;
        job.Operation = operation;
        auto pipe = TPipeFactory().Create();
        job.Reader = pipe.CreateAsyncReader();
        job.OutputFile = std::make_unique<TFile>(FHANDLE(pipe.ReleaseWriteFD()));
        Jobs_.push_back(std::move(job));

        operationSuspendFutures.push_back(operation->GetController()->Suspend());
        operationIds.push_back(operation->GetId());

        LOG_INFO("Snapshot job registered (OperationId: %v)",
            operation->GetId());
    }

    LOG_INFO("Suspending controllers");

    PROFILE_TIMING ("/controllers_suspend_time") {
        auto result = WaitFor(Combine(operationSuspendFutures));
        if (!result.IsOK()) {
            LOG_FATAL(result, "Failed to suspend controllers");
        }
    }

    LOG_INFO("Controllers suspended");

    auto forkFuture = VoidFuture;
    PROFILE_TIMING ("/fork_time") {
        forkFuture = Fork();
    }

    for (const auto& job : Jobs_) {
        job.Operation->GetController()->Resume();
    }

    auto uploadFuture = UploadSnapshots()
        .Apply(
            BIND([operationIds, this, this_ = MakeStrong(this)] (const std::vector<TError>& errors) {
                for (size_t i = 0; i < errors.size(); ++i) {
                    const auto& error = errors[i];
                    if (!error.IsOK()) {
                        LOG_INFO(error, "Failed to build snapshot for operation (OperationId: %v)",
                            operationIds[i]);
                    }
                }
            }));
    return Combine(std::vector<TFuture<void>>{forkFuture, uploadFuture});
}

TDuration TSnapshotBuilder::GetTimeout() const
{
    return Config_->SnapshotTimeout;
}

void TSnapshotBuilder::RunParent()
{
    for (const auto& job : Jobs_) {
        job.OutputFile->Close();
    }
}

void DoSnapshotJobs(const std::vector<TSnapshotJob> Jobs_)
{
    for (const auto& job : Jobs_) {
        TFileOutput outputStream(*job.OutputFile);
        TBufferedOutput bufferedOutput(&outputStream, PipeWriteBufferSize);
        try {
            job.Operation->GetController()->SaveSnapshot(&bufferedOutput);
            bufferedOutput.Finish();
            job.OutputFile->Close();
        } catch (const TFileError& ex) {
            // Failed to save snapshot because other side of the pipe was closed.
        }
    }
}

void TSnapshotBuilder::RunChild()
{
    std::vector<int> descriptors = {2};
    for (const auto& job : Jobs_) {
        descriptors.push_back(int(job.OutputFile->GetHandle()));
    }
    CloseAllDescriptors(descriptors);

    std::vector<std::thread> builderThreads;
    {
        const int jobsPerBuilder = Jobs_.size() / Config_->ParallelSnapshotBuilderCount + 1;
        std::vector<TSnapshotJob> jobs;
        for (int jobIndex = 0; jobIndex < Jobs_.size(); ++jobIndex) {
            auto& job = Jobs_[jobIndex];
            TSnapshotJob snapshotJob;
            snapshotJob.Operation = std::move(job.Operation);
            snapshotJob.OutputFile = std::move(job.OutputFile);
            jobs.push_back(std::move(snapshotJob));

            if (jobs.size() >= jobsPerBuilder || jobIndex + 1 == Jobs_.size()) {
                builderThreads.emplace_back(
                    DoSnapshotJobs, std::move(jobs));
                jobs.clear();
            }
        }
        Jobs_.clear();
    }

    for (auto& builderThread : builderThreads) {
        builderThread.join();
    }
}

TFuture<std::vector<TError>> TSnapshotBuilder::UploadSnapshots()
{
    std::vector<TFuture<void>> snapshotUploadFutures;
    for (auto& job : Jobs_) {
        auto controller = job.Operation->GetController();
        auto cancelableInvoker = controller->GetCancelableContext()->CreateInvoker(
            Scheduler_->GetSnapshotIOInvoker());
        auto uploadFuture = BIND(
            &TSnapshotBuilder::UploadSnapshot,
            MakeStrong(this),
            Passed(std::move(job)))
                .AsyncVia(cancelableInvoker)
                .Run();
        snapshotUploadFutures.push_back(std::move(uploadFuture));
    }
    return CombineAll(snapshotUploadFutures);
}

void TSnapshotBuilder::UploadSnapshot(const TJob& job)
{
    const auto& operationId = job.Operation->GetId();

    auto Logger = this->Logger;
    Logger.AddTag("OperationId: %v", operationId);

    try {
        LOG_INFO("Started uploading snapshot");

        auto snapshotPath = GetSnapshotPath(operationId);

        // Start outer transaction.
        ITransactionPtr transaction;
        {
            TTransactionStartOptions options;
            auto attributes = CreateEphemeralAttributes();
            attributes->Set(
                "title",
                Format("Snapshot upload for operation %v", operationId));
            options.Attributes = std::move(attributes);
            auto transactionOrError = WaitFor(
                Client_->StartTransaction(
                NTransactionClient::ETransactionType::Master,
                options));
            transaction = transactionOrError.ValueOrThrow();
        }

        // Remove previous snapshot, if exists.
        {
            TRemoveNodeOptions options;
            options.Force = true;
            auto result = WaitFor(transaction->RemoveNode(
                snapshotPath,
                options));
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error removing previous snapshot");
        }

        // Create new snapshot node.
        {
            TCreateNodeOptions options;
            auto attributes = CreateEphemeralAttributes();
            attributes->Set("version", GetCurrentSnapshotVersion());
            options.Attributes = std::move(attributes);
            auto result = WaitFor(transaction->CreateNode(
                snapshotPath,
                EObjectType::File,
                options));
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error creating snapshot node");
        }

        // Upload new snapshot.
        {
            TFileWriterOptions options;
            options.Config = Config_->SnapshotWriter;
            auto writer = transaction->CreateFileWriter(snapshotPath, options);

            WaitFor(writer->Open())
                .ThrowOnError();

            struct TSnapshotBuilderBufferTag { };
            auto buffer = TSharedMutableRef::Allocate<TSnapshotBuilderBufferTag>(RemoteWriteBufferSize, false);

            while (true) {
                size_t bytesRead = WaitFor(job.Reader->Read(buffer))
                    .ValueOrThrow();

                if (bytesRead == 0) {
                    break;
                }

                WaitFor(writer->Write(buffer.Slice(0, bytesRead)))
                    .ThrowOnError();
            }

            WaitFor(writer->Close())
                .ThrowOnError();

            LOG_INFO("Snapshot uploaded successfully");
        }

        // Commit outer transaction.
        WaitFor(transaction->Commit())
            .ThrowOnError();
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Error uploading snapshot");
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
