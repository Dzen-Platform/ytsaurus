#include "log_manager.h"
#include "private.h"
#include "config.h"
#include "log.h"
#include "writer.h"

#include <yt/core/concurrency/fork_aware_spinlock.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/scheduler_thread.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/hash.h>
#include <yt/core/misc/lock_free.h>
#include <yt/core/misc/pattern_formatter.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/raw_formatter.h>
#include <yt/core/misc/singleton.h>
#include <yt/core/misc/shutdown.h>
#include <yt/core/misc/variant.h>
#include <yt/core/misc/ref_counted_tracker.h>

#include <yt/core/profiling/profile_manager.h>
#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/timing.h>

#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/ypath_service.h>
#include <yt/core/ytree/yson_serializable.h>
#include <yt/core/ytree/convert.h>

#include <util/system/defaults.h>
#include <util/system/sigset.h>
#include <util/system/yield.h>
#include <util/system/tls.h>

#include <util/string/vector.h>

#include <atomic>
#include <mutex>

#ifdef _win_
    #include <io.h>
#else
    #include <unistd.h>
#endif

#ifdef _linux_
    #include <sys/inotify.h>
#endif

#include <errno.h>

namespace NYT::NLogging {

using namespace NYTree;
using namespace NConcurrency;
using namespace NProfiling;
using namespace NTracing;

////////////////////////////////////////////////////////////////////////////////

static const TLogger Logger(SystemLoggingCategoryName);
static const auto& Profiler = LoggingProfiler;

static constexpr auto ProfilingPeriod = TDuration::Seconds(10);
static constexpr auto DequeuePeriod = TDuration::MilliSeconds(100);
static constexpr int PerThreadBatchingReserveCapacity = 256;

static __thread TDuration PerThreadBatchingPeriod;
static __thread NProfiling::TCpuInstant PerThreadBatchingDeadline;
static __thread std::vector<TLogEvent>* PerThreadBatchingEvents;
Y_STATIC_THREAD(std::vector<TLogEvent>) PerThreadBatchingEventsHolder;

////////////////////////////////////////////////////////////////////////////////

bool operator == (const TLogWritersCacheKey& lhs, const TLogWritersCacheKey& rhs)
{
    return lhs.Category == rhs.Category && lhs.LogLevel == rhs.LogLevel && lhs.MessageFormat == rhs.MessageFormat;
}

////////////////////////////////////////////////////////////////////////////////

class TNotificationHandle
    : private TNonCopyable
{
public:
    TNotificationHandle()
        : FD_(-1)
    {
#ifdef _linux_
        FD_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        YCHECK(FD_ >= 0);
#endif
    }

    ~TNotificationHandle()
    {
#ifdef _linux_
        YCHECK(FD_ >= 0);
        ::close(FD_);
#endif
    }

    int Poll()
    {
#ifdef _linux_
        YCHECK(FD_ >= 0);

        char buffer[sizeof(struct inotify_event) + NAME_MAX + 1];
        ssize_t rv = HandleEintr(::read, FD_, buffer, sizeof(buffer));

        if (rv < 0) {
            if (errno != EAGAIN) {
                YT_LOG_ERROR(
                    TError::FromSystem(errno),
                    "Unable to poll inotify() descriptor %v",
                    FD_);
            }
        } else if (rv > 0) {
            Y_ASSERT(rv >= sizeof(struct inotify_event));
            struct inotify_event* event = (struct inotify_event*)buffer;

            if (event->mask & IN_ATTRIB) {
                YT_LOG_TRACE(
                    "Watch %v has triggered metadata change (IN_ATTRIB)",
                    event->wd);
            }
            if (event->mask & IN_DELETE_SELF) {
                YT_LOG_TRACE(
                    "Watch %v has triggered a deletion (IN_DELETE_SELF)",
                    event->wd);
            }
            if (event->mask & IN_MOVE_SELF) {
                YT_LOG_TRACE(
                    "Watch %v has triggered a movement (IN_MOVE_SELF)",
                    event->wd);
            }

            return event->wd;
        } else {
            // Do nothing.
        }
#endif
        return 0;
    }

    DEFINE_BYVAL_RO_PROPERTY(int, FD);
};

////////////////////////////////////////////////////////////////////////////////

class TNotificationWatch
    : private TNonCopyable
{
public:
    TNotificationWatch(
        TNotificationHandle* handle,
        const TString& path,
        TClosure callback)
        : FD_(handle->GetFD())
        , WD_(-1)
        , Path_(path)
        , Callback_(std::move(callback))

    {
        FD_ = handle->GetFD();
        YCHECK(FD_ >= 0);

        CreateWatch();
    }

    ~TNotificationWatch()
    {
        DropWatch();
    }

    DEFINE_BYVAL_RO_PROPERTY(int, FD);
    DEFINE_BYVAL_RO_PROPERTY(int, WD);

    void Run()
    {
        Callback_.Run();
        // Reinitialize watch to hook to the newly created file.
        DropWatch();
        CreateWatch();
    }

private:
    void CreateWatch()
    {
        YCHECK(WD_ <= 0);
#ifdef _linux_
        WD_ = inotify_add_watch(
            FD_,
            Path_.c_str(),
            IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);

        if (WD_ < 0) {
            YT_LOG_ERROR(TError::FromSystem(errno), "Error registering watch for %v",
                Path_);
            WD_ = -1;
        } else if (WD_ > 0) {
            YT_LOG_TRACE("Registered watch %v for %v",
                WD_,
                Path_);
        } else {
            Y_UNREACHABLE();
        }
#else
        WD_ = -1;
#endif
    }

    void DropWatch()
    {
#ifdef _linux_
        if (WD_ > 0) {
            YT_LOG_TRACE("Unregistering watch %v for %v",
                WD_,
                Path_);
            inotify_rm_watch(FD_, WD_);
        }
#endif
        WD_ = -1;
    }

private:
    TString Path_;
    TClosure Callback_;

};

////////////////////////////////////////////////////////////////////////////////

namespace {

void ReloadSignalHandler(int signal)
{
    NLogging::TLogManager::Get()->Reopen();
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

template <class TElement>
class TExpiringSet
{
public:
    TExpiringSet()
    {
        Reconfigure(TDuration::Zero());
    }

    explicit TExpiringSet(TDuration livetime)
    {
        Reconfigure(livetime);
    }

    void Update(std::vector<TElement> elements)
    {
        RemoveExpired();
        Insert(std::move(elements));
    }

    bool Contains(const TElement& element)
    {
        return Set_.contains(element);
    }

    void Reconfigure(TDuration livetime)
    {
        Livetime_ = DurationToCpuDuration(livetime);
    }

    void Clear()
    {
        Set_.clear();
        ExpirationQueue_ = std::priority_queue<TPack>();
    }

private:
    struct TPack
    {
        std::vector<TElement> Elements;
        TCpuInstant ExpirationTime;

        bool operator<(const TPack& other) const
        {
            // Reversed ordering for the priority queue.
            return ExpirationTime > other.ExpirationTime;
        }
    };

    TCpuDuration Livetime_;
    THashSet<TElement> Set_;
    std::priority_queue<TPack> ExpirationQueue_;


    void Insert(std::vector<TElement> elements)
    {
        for (const auto& element : elements) {
            Set_.insert(element);
        }

        ExpirationQueue_.push(TPack{std::move(elements), GetCpuInstant() + Livetime_});
    }

    void RemoveExpired()
    {
        auto now = GetCpuInstant();
        while (!ExpirationQueue_.empty() && ExpirationQueue_.top().ExpirationTime < now) {
            for (const auto& element : ExpirationQueue_.top().Elements) {
                Set_.erase(element);
            }

            ExpirationQueue_.pop();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TLogManager::TImpl
    : public TRefCounted
{
public:
    TImpl()
        : EventQueue_(New<TInvokerQueue>(
            EventCount_,
            NProfiling::EmptyTagIds,
            false,
            false))
        , LoggingThread_(New<TThread>(this))
        , SystemWriters_({New<TStderrLogWriter>()})
    {
        DoUpdateConfig(TLogConfig::CreateDefault());
        SystemCategory_ = GetCategory(SystemLoggingCategoryName);
    }

    void Configure(INodePtr node)
    {
        Configure(TLogConfig::CreateFromNode(node));
    }

    void Configure(TLogConfigPtr config)
    {
        if (LoggingThread_->IsShutdown()) {
            return;
        }

        EnsureStarted();

        TConfigEvent event;
        event.Config = std::move(config);
        LoggerQueue_.Enqueue(event);

        auto future = event.Promise.ToFuture();

        DequeueExecutor_->ScheduleOutOfBand();

        future.Get();
    }

    void ConfigureSimple(
        const char* logLevelStr,
        const char* logExcludeCategoriesStr,
        const char* logIncludeCategoriesStr)
    {
        if (!logLevelStr && !logExcludeCategoriesStr && !logIncludeCategoriesStr) {
            return;
        }

        const char* const stderrWriterName = "stderr";

        auto rule = New<TRuleConfig>();
        rule->Writers.push_back(stderrWriterName);
        rule->MinLevel = ELogLevel::Fatal;

        if (logLevelStr) {
            TString logLevel = logLevelStr;
            if (!logLevel.empty()) {
                // This handles most typical casings like "DEBUG", "debug", "Debug".
                logLevel.to_title();
                rule->MinLevel = TEnumTraits<ELogLevel>::FromString(logLevel);
            }
        }

        std::vector<TString> logExcludeCategories;
        if (logExcludeCategoriesStr) {
            logExcludeCategories = SplitString(logExcludeCategoriesStr, ",");
        }

        for (const auto& excludeCategory : logExcludeCategories) {
            rule->ExcludeCategories.insert(excludeCategory);
        }

        std::vector<TString> logIncludeCategories;
        if (logIncludeCategoriesStr) {
            logIncludeCategories = SplitString(logIncludeCategoriesStr, ",");
        }

        if (!logIncludeCategories.empty()) {
            rule->IncludeCategories.emplace();
            for (const auto& includeCategory : logIncludeCategories) {
                rule->IncludeCategories->insert(includeCategory);
            }
        }

        auto config = New<TLogConfig>();
        config->Rules.push_back(std::move(rule));

        config->MinDiskSpace = 0;
        config->HighBacklogWatermark = std::numeric_limits<int>::max();
        config->LowBacklogWatermark = 0;

        auto stderrWriter = New<TWriterConfig>();
        stderrWriter->Type = EWriterType::Stderr;

        config->WriterConfigs.insert(std::make_pair(stderrWriterName, std::move(stderrWriter)));

        Configure(std::move(config));
    }

    void ConfigureFromEnv()
    {
        ConfigureSimple(
            getenv("YT_LOG_LEVEL"),
            getenv("YT_LOG_EXCLUDE_CATEGORIES"),
            getenv("YT_LOG_INCLUDE_CATEGORIES"));
    }

    void Shutdown()
    {
        ShutdownRequested_ = true;

        if (LoggingThread_->GetId() == ::TThread::CurrentThreadId()) {
            FlushWriters();
        } else {
            // Wait for all previously enqueued messages to be flushed
            // but no more than ShutdownGraceTimeout to prevent hanging.
            Synchronize(TInstant::Now() + Config_->ShutdownGraceTimeout);
        }

        EventQueue_->Shutdown();
        LoggingThread_->Shutdown();
    }

    /*!
     * In some cases (when configuration is being updated at the same time),
     * the actual version is greater than the version returned by this method.
     */
    int GetVersion() const
    {
        return Version_.load();
    }

    const TLoggingCategory* GetCategory(const char* categoryName)
    {
        if (!categoryName) {
            return nullptr;
        }

        TGuard<TForkAwareSpinLock> guard(SpinLock_);
        auto it = NameToCategory_.find(categoryName);
        if (it == NameToCategory_.end()) {
            auto category = std::make_unique<TLoggingCategory>();
            category->Name = categoryName;
            category->ActualVersion = &Version_;
            it = NameToCategory_.emplace(categoryName, std::move(category)).first;
            DoUpdateCategory(it->second.get());
        }
        return it->second.get();
    }

    void UpdateCategory(TLoggingCategory* category)
    {
        TGuard<TForkAwareSpinLock> guard(SpinLock_);
        DoUpdateCategory(category);
    }

    void UpdatePosition(TLoggingPosition* position, TStringBuf message)
    {
        TGuard<TForkAwareSpinLock> guard(SpinLock_);
        DoUpdatePosition(position, message);
    }

    void Enqueue(TLogEvent&& event)
    {
        if (event.Level == ELogLevel::Fatal) {
            bool shutdown = false;
            if (!ShutdownRequested_.compare_exchange_strong(shutdown, true)) {
                // Fatal events should not get out of this call.
                Sleep(TDuration::Max());
            }

            // Collect last-minute information.
            TRawFormatter<1024> formatter;
            formatter.AppendString("\n*** Fatal error ***\n");
            formatter.AppendString(TStringBuf(event.Message.Begin(), event.Message.End()));
            formatter.AppendString("\n*** Aborting ***\n");

            HandleEintr(::write, 2, formatter.GetData(), formatter.GetBytesWritten());

            // Add fatal message to log and notify event log queue.
            PushEvent(std::move(event));

            // Flush everything and die.
            Shutdown();

            std::terminate();
        }

        if (ShutdownRequested_) {
            return;
        }

        if (LoggingThread_->IsShutdown()) {
            return;
        }

        EnsureStarted();

        // Order matters here; inherent race may lead to negative backlog and integer overflow.
        ui64 writtenEvents = WrittenEvents_.load();
        ui64 enqueuedEvents = EnqueuedEvents_.load();
        ui64 backlogEvents = enqueuedEvents - writtenEvents;

        // NB: This is somewhat racy but should work fine as long as more messages keep coming.
        if (Suspended_) {
            if (backlogEvents < LowBacklogWatermark_) {
                Suspended_ = false;
                YT_LOG_INFO("Backlog size has dropped below low watermark %v, logging resumed",
                    LowBacklogWatermark_);
            }
        } else {
            if (backlogEvents >= HighBacklogWatermark_) {
                Suspended_ = true;
                YT_LOG_WARNING("Backlog size has exceeded high watermark %v, logging suspended",
                    HighBacklogWatermark_);
            }
        }

        // NB: Always allow system messages to pass through.
        if (Suspended_ && event.Category != SystemCategory_) {
            return;
        }

        if (PerThreadBatchingPeriod != TDuration::Zero()) {
            BatchEvent(std::move(event));
            if (NProfiling::GetCpuInstant() > PerThreadBatchingDeadline) {
                FlushBatchedEvents();
            }
        } else {
            PushEvent(std::move(event));
        }
    }

    void Reopen()
    {
        ReopenRequested_ = true;
    }

    void SetPerThreadBatchingPeriod(TDuration value)
    {
        if (PerThreadBatchingPeriod == value) {
            return;
        }
        FlushBatchedEvents();
        PerThreadBatchingPeriod = value;
    }

    TDuration GetPerThreadBatchingPeriod() const
    {
        return PerThreadBatchingPeriod;
    }

    void SuppressTrace(TTraceId traceId)
    {
        if (traceId == InvalidTraceId) {
            return;
        }

        if (!TraceSuppressionEnabled_) {
            return;
        }

        SuppressedTraceIdQueue_.Enqueue(traceId);
    }

    void Synchronize(TInstant deadline = TInstant::Max())
    {
        auto enqueuedEvents = EnqueuedEvents_.load();
        while (enqueuedEvents > FlushedEvents_.load() && TInstant::Now() < deadline) {
            SchedYield();
        }
    }

private:
    struct TConfigEvent
    {
        TLogConfigPtr Config;
        TPromise<void> Promise = NewPromise<void>();
    };

    using TLoggerQueueItem = std::variant<
        TLogEvent,
        std::vector<TLogEvent>,
        TConfigEvent
    >;

    class TThread
        : public TSchedulerThread
    {
    public:
        explicit TThread(TImpl* owner)
            : TSchedulerThread(
                owner->EventCount_,
                SystemLoggingCategoryName,
                NProfiling::EmptyTagIds,
                false,
                false)
            , Owner_(owner)
        { }

    private:
        TImpl* const Owner_;

        virtual void OnThreadStart() override
        {
#ifdef _unix_
            // Set mask.
            sigset_t ss;
            sigemptyset(&ss);
            sigaddset(&ss, SIGHUP);
            sigprocmask(SIG_UNBLOCK, &ss, nullptr);

            // Set handler.
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sigemptyset(&sa.sa_mask);
            sa.sa_handler = &ReloadSignalHandler;

            YCHECK(sigaction(SIGHUP, &sa, nullptr) == 0);
#endif
        }

        virtual EBeginExecuteResult BeginExecute() override
        {
            return Owner_->BeginExecute();
        }

        virtual void EndExecute() override
        {
            Owner_->EndExecute();
        }
    };

    EBeginExecuteResult BeginExecute()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        return EventQueue_->BeginExecute(&CurrentAction_);
    }

    void EndExecute()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        EventQueue_->EndExecute(&CurrentAction_);
    }


    void EnsureStarted()
    {
        std::call_once(Started_, [&] {
            if (LoggingThread_->IsShutdown()) {
                return;
            }

            LoggingThread_->Start();
            EventQueue_->SetThreadId(LoggingThread_->GetId());

            ProfilingExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::OnProfiling, MakeStrong(this)),
                ProfilingPeriod);
            ProfilingExecutor_->Start();

            DequeueExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::OnDequeue, MakeStrong(this)),
                DequeuePeriod);
            DequeueExecutor_->Start();
        });
    }

    const std::vector<ILogWriterPtr>& GetWriters(const TLogEvent& event)
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        if (event.Category == SystemCategory_) {
            return SystemWriters_;
        }

        TLogWritersCacheKey cacheKey{event.Category->Name, event.Level, event.MessageFormat};
        auto it = CachedWriters_.find(cacheKey);
        if (it != CachedWriters_.end()) {
            return it->second;
        }

        THashSet<TString> writerIds;
        for (const auto& rule : Config_->Rules) {
            if (rule->IsApplicable(event.Category->Name, event.Level, event.MessageFormat)) {
                writerIds.insert(rule->Writers.begin(), rule->Writers.end());
            }
        }

        std::vector<ILogWriterPtr> writers;
        for (const auto& writerId : writerIds) {
            auto writerIt = Writers_.find(writerId);
            YCHECK(writerIt != Writers_.end());
            writers.push_back(writerIt->second);
        }

        auto pair = CachedWriters_.insert(std::make_pair(cacheKey, writers));
        YCHECK(pair.second);

        return pair.first->second;
    }

    std::unique_ptr<TNotificationWatch> CreateNotificationWatch(ILogWriterPtr writer, const TString& fileName)
    {
#ifdef _linux_
        if (Config_->WatchPeriod) {
            if (!NotificationHandle_) {
                NotificationHandle_.reset(new TNotificationHandle());
            }
            return std::unique_ptr<TNotificationWatch>(
                new TNotificationWatch(
                    NotificationHandle_.get(),
                    fileName.c_str(),
                    BIND(&ILogWriter::Reload, writer)));
        }
#endif
        return nullptr;
    }

    void UpdateConfig(TConfigEvent& event)
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        if (ShutdownRequested_) {
            return;
        }

        if (LoggingThread_->IsShutdown()) {
            return;
        }

        EnsureStarted();

        FlushWriters();

        DoUpdateConfig(event.Config);

        if (FlushExecutor_) {
            FlushExecutor_->Stop();
            FlushExecutor_.Reset();
        }

        if (WatchExecutor_) {
            WatchExecutor_->Stop();
            WatchExecutor_.Reset();
        }

        auto flushPeriod = Config_->FlushPeriod;
        if (flushPeriod) {
            FlushExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::FlushWriters, MakeStrong(this)),
                *flushPeriod);
            FlushExecutor_->Start();
        }

        auto watchPeriod = Config_->WatchPeriod;
        if (watchPeriod) {
            WatchExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::WatchWriters, MakeStrong(this)),
                *watchPeriod);
            WatchExecutor_->Start();
        }

        auto checkSpacePeriod = Config_->CheckSpacePeriod;
        if (checkSpacePeriod) {
            CheckSpaceExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::CheckSpace, MakeStrong(this)),
                *checkSpacePeriod);
            CheckSpaceExecutor_->Start();
        }

        event.Promise.Set();
    }

    void DoUpdateConfig(const TLogConfigPtr& logConfig)
    {
        {
            decltype(Writers_) writers;
            decltype(CachedWriters_) cachedWriters;

            TGuard<TForkAwareSpinLock> guard(SpinLock_);
            Writers_.swap(writers);
            CachedWriters_.swap(cachedWriters);
            Config_ = logConfig;
            HighBacklogWatermark_ = Config_->HighBacklogWatermark;
            LowBacklogWatermark_ = Config_->LowBacklogWatermark;
            TraceSuppressionEnabled_ = Config_->TraceSuppressionTimeout != TDuration::Zero();

            guard.Release();

            // writers and cachedWriters will die here where we don't
            // hold the spinlock anymore.
        }

        if (TraceSuppressionEnabled_) {
            SuppressedTraceIdSet_.Reconfigure((Config_->TraceSuppressionTimeout + DequeuePeriod) * 2);
        } else {
            SuppressedTraceIdSet_.Clear();
            SuppressedTraceIdQueue_.DequeueAll();
        }

        for (const auto& pair : Config_->WriterConfigs) {
            const auto& name = pair.first;
            const auto& config = pair.second;

            ILogWriterPtr writer;
            std::unique_ptr<ILogFormatter> formatter;
            std::unique_ptr<TNotificationWatch> watch;

            switch (config->AcceptedMessageFormat) {
                case ELogMessageFormat::PlainText:
                    formatter = std::make_unique<TPlainTextLogFormatter>();
                    break;
                case ELogMessageFormat::Structured:
                    formatter = std::make_unique<TJsonLogFormatter>();
                    break;
                default:
                    Y_UNREACHABLE();
            }

            switch (config->Type) {
                case EWriterType::Stdout:
                    writer = New<TStdoutLogWriter>(std::move(formatter), name);
                    break;
                case EWriterType::Stderr:
                    writer = New<TStderrLogWriter>(std::move(formatter), name);
                    break;
                case EWriterType::File:
                    writer = New<TFileLogWriter>(std::move(formatter), name, config->FileName, config->EnableCompression);
                    watch = CreateNotificationWatch(writer, config->FileName);
                    break;
                default:
                    Y_UNREACHABLE();
            }

            writer->SetRateLimit(config->RateLimit);
            writer->SetCategoryRateLimits(Config_->CategoryRateLimits);

            YCHECK(Writers_.insert(std::make_pair(name, std::move(writer))).second);

            if (watch) {
                if (watch->GetWD() >= 0) {
                    // Watch can fail to initialize if the writer is disabled
                    // e.g. due to the lack of space.
                    YCHECK(NotificationWatchesIndex_.insert(
                        std::make_pair(watch->GetWD(), watch.get())).second);
                }
                NotificationWatches_.emplace_back(std::move(watch));
            }
        }

        Version_++;
    }

    void WriteEvent(const TLogEvent& event)
    {
        if (ReopenRequested_) {
            ReopenRequested_ = false;
            ReloadWriters();
        }
        LoggingProfiler.Increment(*GetCategoryEventsCounter(event.Category->Name), 1);
        for (const auto& writer : GetWriters(event)) {
            writer->Write(event);
        }
    }

    void WriteEvents(const std::vector<TLogEvent>& events)
    {
        for (const auto& event : events) {
            WriteEvent(event);
        }
    }

    void FlushWriters()
    {
        for (auto& pair : Writers_) {
            pair.second->Flush();
        }
    }

    void ReloadWriters()
    {
        Version_++;
        for (auto& pair : Writers_) {
            pair.second->Reload();
        }
    }

    void CheckSpace()
    {
        for (auto& pair : Writers_) {
            pair.second->CheckSpace(Config_->MinDiskSpace);
        }
    }

    void WatchWriters()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        if (!NotificationHandle_)
            return;

        int previousWD = -1, currentWD = -1;
        while ((currentWD = NotificationHandle_->Poll()) > 0) {
            if (currentWD == previousWD) {
                continue;
            }
            auto&& it = NotificationWatchesIndex_.find(currentWD);
            auto&& jt = NotificationWatchesIndex_.end();
            if (it == jt) {
                continue;
            }

            auto* watch = it->second;
            watch->Run();

            if (watch->GetWD() != currentWD) {
                NotificationWatchesIndex_.erase(it);
                if (watch->GetWD() >= 0) {
                    // Watch can fail to initialize if the writer is disabled
                    // e.g. due to the lack of space.
                    YCHECK(NotificationWatchesIndex_.insert(
                        std::make_pair(watch->GetWD(), watch)).second);
                }
            }

            previousWD = currentWD;
        }
    }

    void PushEvent(TLogEvent&& event)
    {
        ++EnqueuedEvents_;
        LoggerQueue_.Enqueue(std::move(event));
    }

    void PushLogEvents(std::vector<TLogEvent>&& events)
    {
        EnqueuedEvents_ += events.size();
        LoggerQueue_.Enqueue(std::move(events));
    }


    void BatchEvent(TLogEvent&& event)
    {
        if (!PerThreadBatchingEvents) {
            PerThreadBatchingEvents = &PerThreadBatchingEventsHolder;
        }
        PerThreadBatchingEvents->emplace_back(std::move(event));
    }

    void FlushBatchedEvents()
    {
        if (!PerThreadBatchingEvents) {
            PerThreadBatchingEvents = &PerThreadBatchingEventsHolder;
        }
        std::vector<TLogEvent> newEvents;
        newEvents.reserve(PerThreadBatchingReserveCapacity);
        newEvents.swap(*PerThreadBatchingEvents);
        PushLogEvents(std::move(newEvents));
        PerThreadBatchingDeadline = NProfiling::GetCpuInstant() + NProfiling::DurationToCpuDuration(PerThreadBatchingPeriod);
    }


    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        auto writtenEvents = WrittenEvents_.load();
        auto enqueuedEvents = EnqueuedEvents_.load();
        auto suppressedEvents = SuppressedEvents_.load();
        auto messageBuffersSize = TRefCountedTracker::Get()->GetBytesAlive(GetRefCountedTypeKey<NDetail::TMessageBufferTag>());

        Profiler.Enqueue("/enqueued_events", enqueuedEvents, EMetricType::Counter);
        Profiler.Enqueue("/written_events", writtenEvents, EMetricType::Counter);
        Profiler.Enqueue("/backlog_events", enqueuedEvents - writtenEvents, EMetricType::Counter);
        Profiler.Enqueue("/suppressed_events", suppressedEvents, EMetricType::Counter);
        Profiler.Enqueue("/message_buffers_size", messageBuffersSize, EMetricType::Gauge);
    }

    void OnDequeue()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        int eventsWritten = TraceSuppressionEnabled_
            ? DequeueWithTraceSuppressionEnabled()
            : DequeueWithTraceSuppressionDisabled();

        if (eventsWritten == 0) {
            return;
        }

        WrittenEvents_ += eventsWritten;

        if (!Config_->FlushPeriod || ShutdownRequested_) {
            FlushWriters();
            FlushedEvents_ = WrittenEvents_.load();
        }
    }

    int DequeueWithTraceSuppressionDisabled()
    {
        int eventsWritten = ProcessTraceSuppressionBuffer();

        while (LoggerQueue_.DequeueAll(true, [&] (TLoggerQueueItem& item) {
            Visit(item,
                [&] (TConfigEvent& event) {
                    UpdateConfig(event);
                },
                [&] (const TLogEvent& event) {
                    WriteEvent(event);
                    ++eventsWritten;
                },
                [&] (const std::vector<TLogEvent>& events) {
                    WriteEvents(events);
                    eventsWritten += events.size();
                });
        }))
        { }

        return eventsWritten;
    }

    int ProcessTraceSuppressionBuffer()
    {
        if (TraceSuppressionEnabled_) {
            SuppressedTraceIdSet_.Update(SuppressedTraceIdQueue_.DequeueAll());
        }

        auto deadline = GetCpuInstant() - DurationToCpuDuration(Config_->TraceSuppressionTimeout);

        int eventsWritten = 0;
        int suppressed = 0;
        while (!TraceSuppressionBuffer_.empty()) {
            auto& event = TraceSuppressionBuffer_.front();

            if (TraceSuppressionEnabled_ && event.Instant > deadline) {
                break;
            }

            ++eventsWritten;

            if (SuppressedTraceIdSet_.Contains(event.TraceId)) {
                ++suppressed;
            } else {
                WriteEvent(event);
            }

            TraceSuppressionBuffer_.pop_front();
        }

        SuppressedEvents_ += suppressed;

        return eventsWritten;
    }

    void MoveEventsToTraceSuppressionBuffer()
    {
        TraceSuppressionBuffer_.clear();

        LoggerQueue_.DequeueAll(true, [&] (TLoggerQueueItem& item) {
            Visit(std::move(item),
                [&] (TConfigEvent&& event) {
                    UpdateConfig(event);
                },
                [&] (TLogEvent&& event) {
                    TraceSuppressionBuffer_.emplace_back(std::move(event));
                },
                [&] (const std::vector<TLogEvent>& events) {
                    TraceSuppressionBuffer_.insert(
                        TraceSuppressionBuffer_.end(),
                        events.begin(),
                        events.end());
                });
        });

        std::sort(TraceSuppressionBuffer_.begin(), TraceSuppressionBuffer_.end(), [] (const auto& lhs, const auto& rhs) {
            return lhs.Instant < rhs.Instant;
        });
    }

    int DequeueWithTraceSuppressionEnabled()
    {
        int totalEventsWritten = 0;
        int eventsWritten;

        do {
            if (TraceSuppressionBuffer_.empty()) {
                MoveEventsToTraceSuppressionBuffer();
            }

            eventsWritten = ProcessTraceSuppressionBuffer();
            totalEventsWritten += eventsWritten;

        } while (eventsWritten > 0);

        return totalEventsWritten;
    }

    void DoUpdateCategory(TLoggingCategory* category)
    {
        auto level = ELogLevel::Maximum;
        for (const auto& rule : Config_->Rules) {
            if (rule->IsApplicable(category->Name, ELogMessageFormat::PlainText)) {
                level = std::min(level, rule->MinLevel);
            }
        }

        category->MinLevel.store(level, std::memory_order_relaxed);
        category->CurrentVersion.store(GetVersion(), std::memory_order_relaxed);
    }

    void DoUpdatePosition(TLoggingPosition* position, TStringBuf message)
    {
        bool positionEnabled = true;
        for (const auto& prefix : Config_->SuppressedMessages) {
            if (message.StartsWith(prefix)) {
                positionEnabled = false;
                break;
            }
        }

        position->Enabled.store(positionEnabled, std::memory_order_relaxed);
        position->CurrentVersion.store(GetVersion(), std::memory_order_relaxed);
    }

    TMonotonicCounter* GetCategoryEventsCounter(const TString& category)
    {
        auto it = CategoryToEvents_.find(category);
        if (it == CategoryToEvents_.end()) {
            auto tagId = TProfileManager::Get()->RegisterTag("category", category);
            TMonotonicCounter counter("/log_events_enqueued", {tagId});
            it = CategoryToEvents_.insert({category, counter}).first;
        }
        return &it->second;
    }

private:
    const std::shared_ptr<TEventCount> EventCount_ = std::make_shared<TEventCount>();
    const TInvokerQueuePtr EventQueue_;

    const TIntrusivePtr<TThread> LoggingThread_;
    DECLARE_THREAD_AFFINITY_SLOT(LoggingThread);

    TEnqueuedAction CurrentAction_;

    // Configuration.
    TForkAwareSpinLock SpinLock_;
    // Version forces this very module's Logger object to update to our own
    // default configuration (default level etc.).
    std::atomic<int> Version_ = {0};
    TLogConfigPtr Config_;
    THashMap<const char*, std::unique_ptr<TLoggingCategory>> NameToCategory_;
    const TLoggingCategory* SystemCategory_;

    // These are just copies from _Config.
    // The values are being read from arbitrary threads but stale values are fine.
    int HighBacklogWatermark_ = -1;
    int LowBacklogWatermark_ = -1;

    bool Suspended_ = false;
    std::once_flag Started_;

    TMultipleProducerSingleConsumerLockFreeStack<TLoggerQueueItem> LoggerQueue_;
    TMultipleProducerSingleConsumerLockFreeStack<TTraceId> SuppressedTraceIdQueue_;

    std::deque<TLogEvent> TraceSuppressionBuffer_;
    TExpiringSet<TTraceId> SuppressedTraceIdSet_;

    THashMap<TString, TMonotonicCounter> CategoryToEvents_;

    std::atomic<ui64> EnqueuedEvents_ = {0};
    std::atomic<ui64> WrittenEvents_ = {0};
    std::atomic<ui64> FlushedEvents_ = {0};
    std::atomic<ui64> SuppressedEvents_ = {0};

    THashMap<TString, ILogWriterPtr> Writers_;
    THashMap<TLogWritersCacheKey, std::vector<ILogWriterPtr>> CachedWriters_;
    std::vector<ILogWriterPtr> SystemWriters_;

    std::atomic<bool> ReopenRequested_ = {false};
    std::atomic<bool> ShutdownRequested_ = {false};
    std::atomic<bool> TraceSuppressionEnabled_ = {false};

    TPeriodicExecutorPtr FlushExecutor_;
    TPeriodicExecutorPtr WatchExecutor_;
    TPeriodicExecutorPtr CheckSpaceExecutor_;
    TPeriodicExecutorPtr ProfilingExecutor_;
    TPeriodicExecutorPtr DequeueExecutor_;

    std::unique_ptr<TNotificationHandle> NotificationHandle_;
    std::vector<std::unique_ptr<TNotificationWatch>> NotificationWatches_;
    THashMap<int, TNotificationWatch*> NotificationWatchesIndex_;
};

////////////////////////////////////////////////////////////////////////////////

TLogManager::TLogManager()
    : Impl_(New<TImpl>())
{ }

TLogManager::~TLogManager() = default;

TLogManager* TLogManager::Get()
{
    return Singleton<TLogManager>();
}

void TLogManager::StaticShutdown()
{
    Get()->Shutdown();
}

void TLogManager::Configure(TLogConfigPtr config)
{
    Impl_->Configure(std::move(config));
}

void TLogManager::ConfigureFromEnv()
{
    Impl_->ConfigureFromEnv();
}

void TLogManager::Shutdown()
{
    Impl_->Shutdown();
}

int TLogManager::GetVersion() const
{
    return Impl_->GetVersion();
}

const TLoggingCategory* TLogManager::GetCategory(const char* categoryName)
{
    return Impl_->GetCategory(categoryName);
}

void TLogManager::UpdateCategory(TLoggingCategory* category)
{
    Impl_->UpdateCategory(category);
}

void TLogManager::UpdatePosition(TLoggingPosition* position, TStringBuf message)
{
    Impl_->UpdatePosition(position, message);
}

void TLogManager::Enqueue(TLogEvent&& event)
{
    Impl_->Enqueue(std::move(event));
}

void TLogManager::Reopen()
{
    Impl_->Reopen();
}

void TLogManager::SetPerThreadBatchingPeriod(TDuration value)
{
    Impl_->SetPerThreadBatchingPeriod(value);
}

TDuration TLogManager::GetPerThreadBatchingPeriod() const
{
    return Impl_->GetPerThreadBatchingPeriod();
}

void TLogManager::SuppressTrace(TTraceId traceId)
{
    Impl_->SuppressTrace(traceId);
}

void TLogManager::Synchronize(TInstant deadline)
{
    Impl_->Synchronize(deadline);
}

////////////////////////////////////////////////////////////////////////////////

REGISTER_SHUTDOWN_CALLBACK(5, TLogManager::StaticShutdown);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLogging
