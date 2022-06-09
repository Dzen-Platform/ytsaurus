#include "file_changelog_dispatcher.h"

#include "private.h"
#include "unbuffered_file_changelog.h"
#include "file_changelog.h"
#include "config.h"

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/misc/collection_helpers.h>
#include <yt/yt/core/misc/finally.h>
#include <yt/yt/core/misc/fs.h>

#include <yt/yt/core/profiling/timing.h>

#include <yt/yt/library/profiling/sensor.h>


#include <atomic>

namespace NYT::NHydra {

using namespace NConcurrency;
using namespace NHydra::NProto;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = HydraLogger;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TFileChangelogDispatcher)
DECLARE_REFCOUNTED_CLASS(TFileChangelogQueue)
DECLARE_REFCOUNTED_CLASS(TFileChangelog)

IFileChangelogPtr CreateFileChangelog(
    int id,
    TFileChangelogDispatcherPtr dispatcher,
    TFileChangelogConfigPtr config,
    IUnbufferedFileChangelogPtr unbufferedChangelog);

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogQueue
    : public TRefCounted
{
public:
    TFileChangelogQueue(
        IUnbufferedFileChangelogPtr changelog,
        const TProfiler& profiler,
        const IInvokerPtr& invoker)
        : Changelog_(std::move(changelog))
        , Profiler(profiler)
        , Invoker_(NConcurrency::CreateBoundedConcurrencyInvoker(invoker, 1))
        , ProcessQueueCallback_(BIND(&TFileChangelogQueue::Process, MakeWeak(this)))
        , FlushedRecordCount_(Changelog_->GetRecordCount())
        , ChangelogReadIOTimer_(Profiler.Timer("/changelog_read_io_time"))
        , ChangelogReadCopyTimer_(Profiler.Timer("/changelog_read_copy_time"))
        , ChangelogFlushIOTimer_(Profiler.Timer("/changelog_flush_io_time"))
    { }

    ~TFileChangelogQueue()
    {
        YT_LOG_DEBUG("Changelog queue destroyed (Path: %v)",
            Changelog_->GetFileName());
    }

    const IUnbufferedFileChangelogPtr& GetChangelog()
    {
        return Changelog_;
    }

    TFuture<void> AsyncAppend(TRange<TSharedRef> records, i64 byteSize)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TFuture<void> result;
        {
            auto guard = Guard(SpinLock_);
            for (const auto& record : records) {
                AppendQueue_.push_back(record);
            }
            ByteSize_ += byteSize;
            YT_VERIFY(FlushPromise_);
            result = FlushPromise_;
        }

        return result;
    }

    TFuture<void> AsyncFlush()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto guard = Guard(SpinLock_);

        if (FlushQueue_.empty() && AppendQueue_.empty()) {
            return VoidFuture;
        }

        FlushForced_.store(true);
        return FlushPromise_;
    }


    bool HasPendingFlushes()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        const auto& config = Changelog_->GetConfig();

        if (ByteSize_.load() >= config->DataFlushSize) {
            return true;
        }

        if (config->FlushPeriod == TDuration::Zero()) {
            return true;
        }

        if (LastFlushed_.load() + NProfiling::DurationToCpuDuration(config->FlushPeriod) <  NProfiling::GetCpuInstant()) {
            return true;
        }

        if (FlushForced_.load()) {
            return true;
        }

        return false;
    }

    bool HasUnflushedRecords()
    {
        VERIFY_THREAD_AFFINITY(SyncThread);

        return !AppendQueue_.empty() || !FlushQueue_.empty();
    }

    void Truncate(int recordCount)
    {
        VERIFY_THREAD_AFFINITY(SyncThread);

        YT_VERIFY(!HasUnflushedRecords());
        YT_VERIFY(FlushedRecordCount_ >= recordCount);
        FlushedRecordCount_ = recordCount;
    }

    void RunPendingFlushes()
    {
        VERIFY_THREAD_AFFINITY(SyncThread);

        SyncFlush();
    }

    std::vector<TSharedRef> Read(int firstRecordId, int maxRecords, i64 maxBytes)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        std::vector<TSharedRef> records;
        int currentRecordId = firstRecordId;
        int needRecords = maxRecords;
        i64 needBytes = maxBytes;

        auto appendRecord = [&] (TSharedRef record) {
            --needRecords;
            ++currentRecordId;
            needBytes -= record.Size();
            records.push_back(std::move(record));
        };

        auto needMore = [&] {
            return needRecords > 0 && needBytes > 0;
        };

        while (needMore()) {
            auto guard = Guard(SpinLock_);
            if (currentRecordId < FlushedRecordCount_) {
                // Read from disk, w/o spinlock.
                guard.Release();

                TEventTimerGuard guard(ChangelogReadIOTimer_);
                auto diskRecords = Changelog_->Read(currentRecordId, needRecords, needBytes);
                for (auto&& record : diskRecords) {
                    appendRecord(record);
                }
            } else {
                // Read from memory, w/ spinlock.

                auto readFromMemory = [&] (const std::vector<TSharedRef>& memoryRecords, int firstMemoryRecordId) {
                    if (!needMore()) {
                        return;
                    }
                    int memoryIndex = currentRecordId - firstMemoryRecordId;
                    YT_VERIFY(memoryIndex >= 0);
                    while (memoryIndex < std::ssize(memoryRecords) && needMore()) {
                        appendRecord(memoryRecords[memoryIndex++]);
                    }
                };

                TEventTimerGuard guard(ChangelogReadCopyTimer_);
                readFromMemory(FlushQueue_, FlushedRecordCount_);
                readFromMemory(AppendQueue_, FlushedRecordCount_ + FlushQueue_.size());

                // Break since we don't expect more records beyond this point.
                break;
            }
        }

        return records;
    }

    NIO::IIOEngine::TReadRequest MakeChunkFragmentReadRequest(
        const NIO::TChunkFragmentDescriptor& fragmentDescriptor)
    {
        return Changelog_->MakeChunkFragmentReadRequest(fragmentDescriptor);
    }

    const IInvokerPtr& GetInvoker()
    {
        return Invoker_;
    }

    void Wakeup()
    {
        if (ProcessQueueCallbackPending_.load(std::memory_order_relaxed)) {
            return;
        }

        if (!ProcessQueueCallbackPending_.exchange(true)) {
            GetInvoker()->Invoke(ProcessQueueCallback_);
        }
    }

    void Process()
    {
        VERIFY_THREAD_AFFINITY(SyncThread);

        ProcessQueueCallbackPending_.store(false);

        if (HasPendingFlushes()) {
            RunPendingFlushes();
        }
    }

private:
    const IUnbufferedFileChangelogPtr Changelog_;
    const TProfiler Profiler;
    const IInvokerPtr Invoker_;
    const TClosure ProcessQueueCallback_;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);

    //! Number of records flushed to the underlying sync changelog.
    int FlushedRecordCount_ = 0;
    //! These records are currently being flushed to the underlying sync changelog and
    //! immediately follow the flushed part.
    std::vector<TSharedRef> FlushQueue_;
    //! Newly appended records go here. These records immediately follow the flush part.
    std::vector<TSharedRef> AppendQueue_;

    std::atomic<i64> ByteSize_ = 0;

    TPromise<void> FlushPromise_ = NewPromise<void>();
    std::atomic<bool> FlushForced_ = false;
    std::atomic<NProfiling::TCpuInstant> LastFlushed_ = 0;
    std::atomic<bool> ProcessQueueCallbackPending_ = false;

    TEventTimer ChangelogReadIOTimer_;
    TEventTimer ChangelogReadCopyTimer_;
    TEventTimer ChangelogFlushIOTimer_;

    DECLARE_THREAD_AFFINITY_SLOT(SyncThread);


    void SyncFlush()
    {
        VERIFY_THREAD_AFFINITY(SyncThread);

        TPromise<void> flushPromise;
        {
            auto guard = Guard(SpinLock_);

            YT_VERIFY(FlushQueue_.empty());
            FlushQueue_.swap(AppendQueue_);
            ByteSize_.store(0);

            YT_VERIFY(FlushPromise_);
            flushPromise = FlushPromise_;
            FlushPromise_ = NewPromise<void>();
            FlushForced_.store(false);
        }

        TError error;
        if (!FlushQueue_.empty()) {
            TEventTimerGuard guard(ChangelogFlushIOTimer_);
            try {
                Changelog_->Append(FlushedRecordCount_, FlushQueue_);
                Changelog_->Flush();
                LastFlushed_.store(NProfiling::GetCpuInstant());
            } catch (const std::exception& ex) {
                error = ex;
            }
        }

        {
            auto guard = Guard(SpinLock_);
            FlushedRecordCount_ += FlushQueue_.size();
            FlushQueue_.clear();
        }

        flushPromise.Set(error);
    }
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogQueue)

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogDispatcher
    : public IFileChangelogDispatcher
{
public:
    TFileChangelogDispatcher(
        NIO::IIOEnginePtr ioEngine,
        IMemoryUsageTrackerPtr memoryUsageTracker,
        TFileChangelogDispatcherConfigPtr config,
        TString threadName,
        TProfiler profiler)
        : IOEngine_(std::move(ioEngine))
        , MemoryUsageTracker_(std::move(memoryUsageTracker))
        , Config_(std::move(config))
        , ProcessQueuesCallback_(BIND(&TFileChangelogDispatcher::ProcessQueues, MakeWeak(this)))
        , ActionQueue_(New<TActionQueue>(std::move(threadName)))
        , PeriodicExecutor_(New<TPeriodicExecutor>(
            ActionQueue_->GetInvoker(),
            ProcessQueuesCallback_,
            Config_->FlushQuantum))
        , Profiler(std::move(profiler))
        , RecordCounter_(Profiler.Counter("/records"))
        , ByteCounter_(Profiler.Counter("/bytes"))
        , QueueCountGauge_(Profiler.Gauge("/queue_count"))
        , ChangelogTruncateIOTimer_(Profiler.Timer("/changelog_truncate_io_time"))
        , ChangelogCloseIOTimer_(Profiler.Timer("/changelog_close_io_time"))
        , ChangelogReadRecordCountGauge_(Profiler.Gauge("/changelog_read_record_count"))
        , ChangelogReadSizeGauge_(Profiler.Gauge("/changelog_read_size"))
    {
        PeriodicExecutor_->Start();
    }

    ~TFileChangelogDispatcher()
    {
        PeriodicExecutor_->Stop();
        ActionQueue_->Shutdown();
    }

    IInvokerPtr GetInvoker() override
    {
        return ActionQueue_->GetInvoker();
    }

    TFuture<IFileChangelogPtr> CreateChangelog(
        int id,
        const TString& path,
        const TChangelogMeta& meta,
        const TFileChangelogConfigPtr& config) override
    {
        return BIND(&TFileChangelogDispatcher::DoCreateChangelog, MakeStrong(this))
            .AsyncVia(ActionQueue_->GetInvoker())
            .Run(id, path, meta, config)
            .ToUncancelable();
    }

    TFuture<IFileChangelogPtr> OpenChangelog(
        int id,
        const TString& path,
        const TFileChangelogConfigPtr& config) override
    {
        return BIND(&TFileChangelogDispatcher::DoOpenChangelog, MakeStrong(this))
            .AsyncVia(ActionQueue_->GetInvoker())
            .Run(id, path, config)
            .ToUncancelable();
    }

    TFuture<void> FlushChangelogs() override
    {
        return BIND(&TFileChangelogDispatcher::DoFlushChangelogs, MakeStrong(this))
            .AsyncVia(ActionQueue_->GetInvoker())
            .Run()
            .ToUncancelable();
    }


    TFileChangelogQueuePtr CreateQueue(IUnbufferedFileChangelogPtr changelog)
    {
        return New<TFileChangelogQueue>(std::move(changelog), Profiler, ActionQueue_->GetInvoker());
    }

    void RegisterQueue(const TFileChangelogQueuePtr& queue)
    {
        queue->GetInvoker()->Invoke(BIND(&TFileChangelogDispatcher::DoRegisterQueue, MakeStrong(this), queue));
    }

    void UnregisterQueue(const TFileChangelogQueuePtr& queue)
    {
        queue->GetInvoker()->Invoke(BIND(&TFileChangelogDispatcher::DoUnregisterQueue, MakeStrong(this), queue));
    }

    TFuture<void> AppendToQueue(const TFileChangelogQueuePtr& queue, TRange<TSharedRef> records, i64 byteSize)
    {
        auto result = queue->AsyncAppend(records, byteSize);
        queue->Wakeup();
        RecordCounter_.Increment(records.Size());
        ByteCounter_.Increment(byteSize);
        return result;
    }

    TFuture<std::vector<TSharedRef>> ReadFromQueue(
        TFileChangelogQueuePtr queue,
        int firstRecordId,
        int maxRecords,
        i64 maxBytes)
    {
        return BIND(&TFileChangelogDispatcher::DoReadFromQueue, MakeStrong(this))
            .AsyncVia(queue->GetInvoker())
            .Run(std::move(queue), firstRecordId, maxRecords, maxBytes);
    }

    TFuture<void> FlushQueue(const TFileChangelogQueuePtr& queue)
    {
        auto result = queue->AsyncFlush();
        queue->Wakeup();
        return result;
    }

    TFuture<void> ForceFlushQueue(const TFileChangelogQueuePtr& queue)
    {
        auto result = queue->AsyncFlush();
        queue->GetInvoker()->Invoke(BIND(&TFileChangelogQueue::Process, queue));
        return result;
    }

    TFuture<void> TruncateQueue(const TFileChangelogQueuePtr& queue, int recordCount)
    {
        return BIND(&TFileChangelogDispatcher::DoTruncateQueue, MakeStrong(this))
            .AsyncVia(queue->GetInvoker())
            .Run(queue, recordCount);
    }

    TFuture<void> CloseQueue(const TFileChangelogQueuePtr& queue)
    {
        return BIND(&TFileChangelogDispatcher::DoCloseQueue, MakeStrong(this))
            .AsyncVia(queue->GetInvoker())
            .Run(queue);
    }

private:
    const NIO::IIOEnginePtr IOEngine_;
    const IMemoryUsageTrackerPtr MemoryUsageTracker_;
    const TFileChangelogDispatcherConfigPtr Config_;
    const TClosure ProcessQueuesCallback_;

    const TActionQueuePtr ActionQueue_;
    const TPeriodicExecutorPtr PeriodicExecutor_;

    const TProfiler Profiler;

    THashSet<TFileChangelogQueuePtr> Queues_;

    TCounter RecordCounter_;
    TCounter ByteCounter_;
    TGauge QueueCountGauge_;
    TEventTimer ChangelogTruncateIOTimer_;
    TEventTimer ChangelogCloseIOTimer_;
    TGauge ChangelogReadRecordCountGauge_;
    TGauge ChangelogReadSizeGauge_;

    void ProcessQueues()
    {
        for (const auto& queue : Queues_) {
            queue->Wakeup();
        }
    }

    void DoRegisterQueue(const TFileChangelogQueuePtr& queue)
    {
        InsertOrCrash(Queues_, queue);
        ProfileQueues();

        YT_LOG_DEBUG("Changelog queue registered (Path: %v)",
            queue->GetChangelog()->GetFileName());

        // See wakeup.
        queue->Process();
    }

    void DoUnregisterQueue(const TFileChangelogQueuePtr& queue)
    {
        YT_VERIFY(!queue->HasUnflushedRecords());

        EraseOrCrash(Queues_, queue);
        ShrinkHashTable(&Queues_);
        ProfileQueues();

        YT_LOG_DEBUG("Changelog queue unregistered (Path: %v)",
            queue->GetChangelog()->GetFileName());
    }

    void ProfileQueues()
    {
        QueueCountGauge_.Update(Queues_.size());
    }

    std::vector<TSharedRef> DoReadFromQueue(
        const TFileChangelogQueuePtr& queue,
        int firstRecordId,
        int maxRecords,
        i64 maxBytes)
    {
        auto records = queue->Read(firstRecordId, maxRecords, maxBytes);
        ChangelogReadRecordCountGauge_.Update(records.size());
        ChangelogReadSizeGauge_.Update(GetByteSize(records));
        return records;
    }

    void DoTruncateQueue(
        const TFileChangelogQueuePtr& queue,
        int recordCount)
    {
        YT_VERIFY(!queue->HasUnflushedRecords());

        TEventTimerGuard guard(ChangelogTruncateIOTimer_);
        const auto& changelog = queue->GetChangelog();
        changelog->Truncate(recordCount);
        queue->Truncate(recordCount);
    }

    void DoCloseQueue(const TFileChangelogQueuePtr& queue)
    {
        YT_VERIFY(!queue->HasUnflushedRecords());

        TEventTimerGuard guard(ChangelogCloseIOTimer_);
        const auto& changelog = queue->GetChangelog();
        changelog->Close();
    }

    IFileChangelogPtr DoCreateChangelog(
        int id,
        const TString& path,
        const TChangelogMeta& meta,
        const TFileChangelogConfigPtr& config)
    {
        auto unbufferedChangelog = CreateUnbufferedFileChangelog(IOEngine_, MemoryUsageTracker_, path, config);
        unbufferedChangelog->Create(meta);
        return CreateFileChangelog(id, this, config, unbufferedChangelog);
    }

    IFileChangelogPtr DoOpenChangelog(
        int id,
        const TString& path,
        const TFileChangelogConfigPtr& config)
    {
        auto unbufferedChangelog = CreateUnbufferedFileChangelog(IOEngine_, MemoryUsageTracker_, path, config);
        unbufferedChangelog->Open();
        return CreateFileChangelog(id, this, config, unbufferedChangelog);
    }

    TFuture<void> DoFlushChangelogs()
    {
        std::vector<TFuture<void>> flushResults;
        for (const auto& queue : Queues_) {
            flushResults.push_back(queue->AsyncFlush());
        }
        return AllSucceeded(flushResults);
    }
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogDispatcher)

////////////////////////////////////////////////////////////////////////////////

class TFileChangelog
    : public IFileChangelog
{
public:
    TFileChangelog(
        int id,
        TFileChangelogDispatcherPtr dispatcher,
        TFileChangelogConfigPtr config,
        IUnbufferedFileChangelogPtr unbufferedChangelog)
        : Id_(id)
        , Dispatcher_(std::move(dispatcher))
        , Config_(std::move(config))
        , Queue_(Dispatcher_->CreateQueue(unbufferedChangelog))
        , RecordCount_(unbufferedChangelog->GetRecordCount())
        , DataSize_(unbufferedChangelog->GetDataSize())
    {
        Dispatcher_->RegisterQueue(Queue_);
    }

    ~TFileChangelog()
    {
        YT_LOG_DEBUG("Destroying changelog queue (Path: %v)",
            Queue_->GetChangelog()->GetFileName());
        Close();
        Dispatcher_->UnregisterQueue(Queue_);
    }

    int GetId() const override
    {
        return Id_;
    }

    int GetRecordCount() const override
    {
        return RecordCount_;
    }

    i64 GetDataSize() const override
    {
        return DataSize_;
    }

    const TChangelogMeta& GetMeta() const override
    {
        return Queue_->GetChangelog()->GetMeta();
    }

    TFuture<void> Append(TRange<TSharedRef> records) override
    {
        YT_VERIFY(!Closed_ && !Truncated_);
        i64 byteSize = GetByteSize(records);
        RecordCount_ += records.Size();
        DataSize_ += byteSize;
        return Dispatcher_->AppendToQueue(Queue_, records, byteSize);
    }

    TFuture<void> Flush() override
    {
        return Dispatcher_->FlushQueue(Queue_);
    }

    TFuture<std::vector<TSharedRef>> Read(
        int firstRecordId,
        int maxRecords,
        i64 maxBytes) const override
    {
        YT_VERIFY(firstRecordId >= 0);
        YT_VERIFY(maxRecords >= 0);
        YT_VERIFY(maxBytes >= 0);
        return Dispatcher_->ReadFromQueue(
            Queue_,
            firstRecordId,
            maxRecords,
            maxBytes);
    }

    NIO::IIOEngine::TReadRequest MakeChunkFragmentReadRequest(
        const NIO::TChunkFragmentDescriptor& fragmentDescriptor) override
    {
        return Queue_->MakeChunkFragmentReadRequest(fragmentDescriptor);
    }

    TFuture<void> Truncate(int recordCount) override
    {
        YT_VERIFY(recordCount <= RecordCount_);
        RecordCount_ = recordCount;
        Truncated_ = true;
        // NB: Ignoring the result seems fine since the changelog
        // will propagate any possible error as the result of all further calls.
        Dispatcher_->ForceFlushQueue(Queue_);
        return Dispatcher_->TruncateQueue(Queue_, recordCount);
    }

    TFuture<void> Close() override
    {
        Closed_ = true;
        // NB: See #Truncate above.
        Dispatcher_->ForceFlushQueue(Queue_);
        return Dispatcher_->CloseQueue(Queue_);
    }

private:
    const int Id_;
    const TFileChangelogDispatcherPtr Dispatcher_;
    const TFileChangelogConfigPtr Config_;

    const TFileChangelogQueuePtr Queue_;

    bool Closed_ = false;
    bool Truncated_ = false;

    std::atomic<int> RecordCount_;
    std::atomic<i64> DataSize_;
};

DEFINE_REFCOUNTED_TYPE(TFileChangelog)

IFileChangelogPtr CreateFileChangelog(
    int id,
    TFileChangelogDispatcherPtr dispatcher,
    TFileChangelogConfigPtr config,
    IUnbufferedFileChangelogPtr unbufferedChangelog)
{
    return New<TFileChangelog>(
        id,
        std::move(dispatcher),
        std::move(config),
        std::move(unbufferedChangelog));
}

////////////////////////////////////////////////////////////////////////////////

IFileChangelogDispatcherPtr CreateFileChangelogDispatcher(
    NIO::IIOEnginePtr ioEngine,
    IMemoryUsageTrackerPtr memoryUsageTracker,
    TFileChangelogDispatcherConfigPtr config,
    TString threadName,
    TProfiler profiler)
{
    return New<TFileChangelogDispatcher>(
        std::move(ioEngine),
        std::move(memoryUsageTracker),
        std::move(config),
        std::move(threadName),
        std::move(profiler));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra

