#include "journal_hunk_chunk_writer.h"

#include "config.h"
#include "journal_chunk_writer.h"

namespace NYT::NJournalClient {

using namespace NApi;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TJournalHunkChunkWriter
    : public IJournalHunkChunkWriter
{
public:
    TJournalHunkChunkWriter(
        NApi::NNative::IClientPtr client,
        TSessionId sessionId,
        TJournalHunkChunkWriterOptionsPtr options,
        TJournalHunkChunkWriterConfigPtr config,
        const NLogging::TLogger& logger)
        : UnderlyingWriter_(CreateJournalChunkWriter(
            std::move(client),
            sessionId,
            options,
            PrepareJournalChunkWriterConfig(config),
            logger))
        , Config_(std::move(config))
        , Logger(logger.WithTag("ChunkId: %v", sessionId.ChunkId))
        , ChunkId_(sessionId.ChunkId)
    {
        auto guard = Guard(Lock_);
        ScheduleCurrentRecordFlush();
    }

    TFuture<void> Open() override
    {
        return UnderlyingWriter_->Open();
    }

    TFuture<std::vector<TJournalHunkDescriptor>> WriteHunks(std::vector<TSharedRef> payloads) override
    {
        std::vector<TJournalHunkDescriptor> descriptors;
        descriptors.reserve(payloads.size());

        std::vector<TFuture<void>> futures;

        bool addFuture = true;
        for (const auto& payload : payloads) {
            auto guard = Guard(Lock_);

            TJournalHunkDescriptor descriptor{
                .ChunkId = ChunkId_,
                .RecordIndex = CurrentRecordIndex_,
                .RecordOffset = CurrentRecordSize_,
                .Size = std::ssize(payload),
            };
            descriptors.push_back(descriptor);

            if (addFuture) {
                futures.push_back(CurrentRecordPromise_.ToFuture());
                addFuture = false;
            }

            ++Statistics_.HunkCount;
            Statistics_.TotalSize += std::ssize(payload);

            CurrentRecordSize_ += std::ssize(payload);
            CurrentRecordHunks_.push_back(std::move(payload));

            // NB: May release guard.
            if (FlushIfNeeded(guard)) {
                addFuture = true;
            }
        }

        return AllSucceeded(std::move(futures))
            .Apply(BIND([descriptors = std::move(descriptors)] { return descriptors; }));
    }

    TJournalHunkChunkWriterStatistics GetStatistics() const override
    {
        auto guard = Guard(Lock_);

        return Statistics_;
    }

    bool IsCloseDemanded() const override
    {
        return UnderlyingWriter_->IsCloseDemanded();
    }

private:
    const IJournalChunkWriterPtr UnderlyingWriter_;

    const TJournalHunkChunkWriterConfigPtr Config_;

    const NLogging::TLogger Logger;

    const TChunkId ChunkId_;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, Lock_);

    std::vector<TSharedRef> CurrentRecordHunks_;

    i64 CurrentRecordSize_ = 0;
    i64 CurrentRecordIndex_ = 0;

    TPromise<void> CurrentRecordPromise_ = NewPromise<void>();

    TDelayedExecutorCookie CurrentRecordFlushCookie_;

    TJournalHunkChunkWriterStatistics Statistics_;

    void ScheduleCurrentRecordFlush()
    {
        VERIFY_SPINLOCK_AFFINITY(Lock_);

        CurrentRecordFlushCookie_ = TDelayedExecutor::Submit(
            BIND(&TJournalHunkChunkWriter::OnRecordFlushTimeout, MakeWeak(this), CurrentRecordIndex_),
            Config_->MaxBatchDelay);
    }

    void OnRecordFlushTimeout(i64 recordIndex)
    {
        auto guard = Guard(Lock_);

        if (CurrentRecordIndex_ != recordIndex) {
            return;
        }

        if (CurrentRecordHunks_.empty()) {
            ScheduleCurrentRecordFlush();
            return;
        }

        // NB: Releases guard.
        FlushCurrentRecord(guard);
    }

    bool FlushIfNeeded(TGuard<NThreading::TSpinLock>& guard)
    {
        VERIFY_SPINLOCK_AFFINITY(Lock_);

        if (CurrentRecordSize_ >= Config_->MaxRecordSize ||
            std::ssize(CurrentRecordHunks_) >= Config_->MaxRecordHunkCount)
        {
            FlushCurrentRecord(guard);
            return true;
        } else {
            return false;
        }
    }

    void FlushCurrentRecord(TGuard<NThreading::TSpinLock>& guard)
    {
        VERIFY_SPINLOCK_AFFINITY(Lock_);
        YT_VERIFY(!CurrentRecordHunks_.empty());

        YT_LOG_DEBUG("Flushing journal hunk chunk record "
            "(RecordIndex: %v, RecordSize: %v, RecordHunkCount: %v)",
            CurrentRecordIndex_,
            CurrentRecordSize_,
            std::ssize(CurrentRecordHunks_));

        struct TJournalHunkChunkWriterTag
        { };
        auto record = MergeRefsToRef<TJournalHunkChunkWriterTag>(std::move(CurrentRecordHunks_));
        auto recordFlushFuture = UnderlyingWriter_->WriteRecord(std::move(record));

        CurrentRecordHunks_.clear();
        CurrentRecordSize_ = 0;
        ++CurrentRecordIndex_;
        TDelayedExecutor::CancelAndClear(CurrentRecordFlushCookie_);

        auto currentRecordPromise = std::exchange(CurrentRecordPromise_, NewPromise<void>());

        ScheduleCurrentRecordFlush();

        guard.Release();

        currentRecordPromise.SetFrom(recordFlushFuture);
    }

    static TJournalChunkWriterConfigPtr PrepareJournalChunkWriterConfig(
        const TJournalHunkChunkWriterConfigPtr& config)
    {
        // Do not batch records in journal chunk writer.
        auto newConfig = CloneYsonSerializable(config);
        newConfig->MaxBatchDelay = TDuration::Zero();
        newConfig->MaxBatchRowCount = 1;

        return newConfig;
    }
};

////////////////////////////////////////////////////////////////////////////////

IJournalHunkChunkWriterPtr CreateJournalHunkChunkWriter(
    NApi::NNative::IClientPtr client,
    TSessionId sessionId,
    TJournalHunkChunkWriterOptionsPtr options,
    TJournalHunkChunkWriterConfigPtr config,
    const NLogging::TLogger& logger)
{
    return New<TJournalHunkChunkWriter>(
        std::move(client),
        sessionId,
        std::move(options),
        std::move(config),
        logger);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJournalClient
