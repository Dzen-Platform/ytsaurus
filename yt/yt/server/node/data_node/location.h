#pragma once

#include "disk_location.h"

#include <yt/yt/server/lib/io/public.h>

#include <yt/yt/ytlib/chunk_client/proto/chunk_info.pb.h>
#include <yt/yt/ytlib/chunk_client/medium_directory.h>

#include <yt/yt/core/actions/signal.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/atomic_object.h>

#include <yt/yt/core/profiling/profiler.h>

#include <yt/yt/library/profiling/sensor.h>

#include <atomic>
#include <map>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ELocationType,
    (Store)
    (Cache)
);

DEFINE_ENUM(EIODirection,
    (Read)
    (Write)
);

DEFINE_ENUM(EIOCategory,
    (Repair)
    (Batch)
    (Interactive)
    (Realtime)
);

////////////////////////////////////////////////////////////////////////////////

struct TLocationPerformanceCounters
    : public TRefCounted
{
    explicit TLocationPerformanceCounters(const NProfiling::TProfiler& profiler);

    TEnumIndexedVector<EIODirection, TEnumIndexedVector<EIOCategory, std::atomic<i64>>> PendingIOSize;
    TEnumIndexedVector<EIODirection, TEnumIndexedVector<EIOCategory, NProfiling::TCounter>> CompletedIOSize;

    NProfiling::TCounter ThrottledReads;
    std::atomic<NProfiling::TCpuInstant> LastReadThrottleTime{};

    void ThrottleRead();

    NProfiling::TCounter ThrottledWrites;
    std::atomic<NProfiling::TCpuInstant> LastWriteThrottleTime{};

    void ThrottleWrite();

    NProfiling::TEventTimer PutBlocksWallTime;
    NProfiling::TEventTimer BlobChunkMetaReadTime;

    NProfiling::TEventTimer BlobChunkWriterOpenTime;
    NProfiling::TEventTimer BlobChunkWriterAbortTime;
    NProfiling::TEventTimer BlobChunkWriterCloseTime;

    TEnumIndexedVector<EWorkloadCategory, NProfiling::TSummary> BlobBlockReadSize;

    TEnumIndexedVector<EWorkloadCategory, NProfiling::TEventTimer> BlobBlockReadTime;
    NProfiling::TCounter BlobBlockReadBytes;
    NProfiling::TCounter BlobBlockReadCount;

    TEnumIndexedVector<EWorkloadCategory, NProfiling::TEventTimer> BlobBlockReadLatencies;
    TEnumIndexedVector<EWorkloadCategory, NProfiling::TEventTimer> BlobChunkMetaReadLatencies;

    NProfiling::TSummary BlobBlockWriteSize;
    NProfiling::TEventTimer BlobBlockWriteTime;
    NProfiling::TCounter BlobBlockWriteBytes;

    NProfiling::TSummary JournalBlockReadSize;
    NProfiling::TEventTimer JournalBlockReadTime;
    NProfiling::TCounter JournalBlockReadBytes;

    NProfiling::TEventTimer JournalChunkCreateTime;
    NProfiling::TEventTimer JournalChunkOpenTime;
    NProfiling::TEventTimer JournalChunkRemoveTime;

    TEnumIndexedVector<ESessionType, std::atomic<int>> SessionCount;

    NProfiling::TGauge UsedSpace;
    NProfiling::TGauge AvailableSpace;
    NProfiling::TGauge ChunkCount;
    NProfiling::TGauge Full;
};

DEFINE_REFCOUNTED_TYPE(TLocationPerformanceCounters)

////////////////////////////////////////////////////////////////////////////////

class TChunkLocation
    : public TDiskLocation
{
public:
    //! Raised when location becomes disabled.
    // NB: This signal can be raised in different threads.
    DEFINE_SIGNAL(void(), Disabled);

public:
    TChunkLocation(
        ELocationType type,
        const TString& id,
        TStoreLocationConfigBasePtr config,
        NClusterNode::TClusterNodeDynamicConfigManagerPtr dynamicConfigManager,
        TChunkStorePtr chunkStore,
        TChunkContextPtr chunkContext,
        IChunkStoreHostPtr chunkStoreHost);

    //! Returns the type.
    ELocationType GetType() const;

    const TStoreLocationConfigBasePtr& GetConfig() const;

    //! Returns the universally unique id.
    TChunkLocationUuid GetUuid() const;

    //! Returns the disk family
    const TString& GetDiskFamily() const;

    //! Returns the IO Engine.
    const NIO::IIOEnginePtr& GetIOEngine() const;

    void UpdateIOEngineType(NIO::EIOEngineType type);

    //! Returns the IO Engine with stats observer.
    const NIO::IIOEngineWorkloadModelPtr& GetIOEngineModel() const;

    //! Returns direct IO policy for read requests.
    NIO::EDirectIOPolicy UseDirectIOForReads() const;

    //! Return the maximum number of bytes in the gap between two adjacent read locations
    //! in order to join them together during read coalescing.
    i64 GetCoalescedReadMaxGapSize() const;

    //! Returns the medium name.
    TString GetMediumName() const;

    //! Sets medium name and reconfigures medium descriptors using given medium directory.
    //! Returns |true| if location medium was changed.
    bool UpdateMediumName(
        const TString& newMediumName,
        const NChunkClient::TMediumDirectoryPtr& mediumDirectory,
        bool onInitialize);

    //! Sets medium descriptor.
    //! #onInitialize indicates whether this method called before any data node heartbeat or on heartbeat response.
    void UpdateMediumDescriptor(
        const NChunkClient::TMediumDescriptor& mediumDescriptor,
        bool onInitialize);

    //! Returns the medium descriptor.
    NChunkClient::TMediumDescriptor GetMediumDescriptor() const;

    const NProfiling::TProfiler& GetProfiler() const;

    //! Returns various performance counters.
    TLocationPerformanceCounters& GetPerformanceCounters();

    //! Returns the root path of the location.
    const TString& GetPath() const;

    //! Returns the maximum number of bytes the chunks assigned to this location
    //! are allowed to use.
    i64 GetQuota() const;

    //! Returns an invoker for various auxiliarly IO activities.
    const IInvokerPtr& GetAuxPoolInvoker();

    //! Scan the location directory removing orphaned files and returning the list of found chunks.
    /*!
     *  If the scan fails, the location becomes disabled and an empty list is returned.
     */
    std::vector<TChunkDescriptor> Scan();

    //! Prepares the location to accept new writes.
    /*!
     *  Must be called when all locations are scanned and all existing chunks are registered.
     *  On failure, acts similarly to Scan.
     */
    void Start();

    //! Marks the location as disabled by attempting to create a lock file and marking assinged chunks
    //! as unavailable.
    void Disable(const TError& reason);

    //! Wraps a given #callback with try/catch block that intercepts all exceptions
    //! and calls #Disable when one happens.
    template <class T>
    TCallback<T()> DisableOnError(const TCallback<T()> callback);

    //! Updates #UsedSpace and #AvailableSpace
    void UpdateUsedSpace(i64 size);

    //! Returns the number of bytes used at the location.
    /*!
     *  \note
     *  This may exceed #GetQuota.
     */
    i64 GetUsedSpace() const;

    //! Updates #AvailableSpace with a system call and returns the result.
    //! Never throws.
    i64 GetAvailableSpace() const;

    //! Returns the number of bytes pending for disk IO.
    i64 GetPendingIOSize(
        EIODirection direction,
        const TWorkloadDescriptor& workloadDescriptor) const;

    //! Returns the maximum number of bytes pending for disk IO in given #direction.
    i64 GetMaxPendingIOSize(EIODirection direction) const;

    //! Acquires a lock for the given number of bytes to be read or written.
    TPendingIOGuard IncreasePendingIOSize(
        EIODirection direction,
        const TWorkloadDescriptor& workloadDescriptor,
        i64 delta);

    //! Increases number of bytes done for disk IO.
    void IncreaseCompletedIOSize(
        EIODirection direction,
        const TWorkloadDescriptor& workloadDescriptor,
        i64 delta);

    //! Changes the number of currently active sessions of a given #type by a given #delta.
    void UpdateSessionCount(ESessionType type, int delta);

    //! Changes the number of chunks by a given delta.
    void UpdateChunkCount(int delta);

    //! Returns the number of currently active sessions of a given #type.
    int GetSessionCount(ESessionType type) const;

    //! Returns the number of currently active sessions of any type.
    int GetSessionCount() const;

    //! Returns the number of chunks.
    int GetChunkCount() const;

    //! Returns a full path for a primary chunk file.
    TString GetChunkPath(TChunkId chunkId) const;

    //! Permanently removes the files comprising a given chunk.
    void RemoveChunkFilesPermanently(TChunkId chunkId);

    //! Removes a chunk permanently or moves it to the trash (if available).
    virtual void RemoveChunkFiles(TChunkId chunkId, bool force);

    NConcurrency::IThroughputThrottlerPtr GetOutThrottler(const TWorkloadDescriptor& descriptor) const;

    //! Returns |true| if reads were throttled (within some recent time interval).
    bool IsReadThrottling() const;

    //! Returns |true| if writes were throttled (within some recent time interval).
    bool IsWriteThrottling() const;

    //! Returns the total number of bytes to read from disk including those accounted by out throttler.
    i64 GetReadQueueSize(const TWorkloadDescriptor& workloadDescriptor) const;

    //! Returns |true| if writes must currently be throttled.
    bool CheckReadThrottling(const TWorkloadDescriptor& workloadDescriptor, bool incrementCounter = true) const;

    //! Returns |true| if writes must currently be throttled.
    bool CheckWriteThrottling(const TWorkloadDescriptor& workloadDescriptor) const;

    //! Returns |true| if location is sick.
    bool IsSick() const;

    //! Returns |true| if location does not contain
    //! files corresponding to given chunk id.
    bool TryLock(TChunkId chunkId, bool verbose = true);

    //! Called when all the chunk files are destroyed.
    void Unlock(TChunkId chunkId);

    const TChunkStorePtr& GetChunkStore() const;

protected:
    const NClusterNode::TClusterNodeDynamicConfigManagerPtr DynamicConfigManager_;
    const TChunkStorePtr ChunkStore_;
    const TChunkContextPtr ChunkContext_;
    const IChunkStoreHostPtr ChunkStoreHost_;

    NProfiling::TProfiler Profiler_;

    static TString GetRelativeChunkPath(TChunkId chunkId);
    static void ForceHashDirectories(const TString& rootPath);

    virtual bool ShouldSkipFileName(const TString& fileName) const;

    virtual void DoStart();
    virtual std::vector<TChunkDescriptor> DoScan();

    i64 GetReadThrottlingLimit() const;
    i64 GetWriteThrottlingLimit() const;

private:
    friend class TPendingIOGuard;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    const ELocationType Type_;
    const TStoreLocationConfigBasePtr Config_;

    TChunkLocationUuid Uuid_;

    TAtomicObject<TError> LocationDisabledAlert_;
    TAtomicObject<TError> MediumAlert_;

    TAtomicObject<NChunkClient::TMediumDescriptor> MediumDescriptor_;
    NProfiling::TGauge MediumTag_;

    mutable std::atomic<i64> AvailableSpace_ = 0;
    std::atomic<i64> UsedSpace_ = 0;
    TEnumIndexedVector<ESessionType, std::atomic<int>> PerTypeSessionCount_;
    std::atomic<int> ChunkCount_ = 0;

    NConcurrency::IThroughputThrottlerPtr ReplicationOutThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletCompactionAndPartitioningOutThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletLoggingOutThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletPreloadOutThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletRecoveryOutThrottler_;
    NConcurrency::IThroughputThrottlerPtr UnlimitedOutThrottler_;

    NIO::IIOEnginePtr IOEngine_;
    NIO::IIOEngineWorkloadModelPtr IOEngineModel_;
    NIO::IDynamicIOEnginePtr DynamicIOEngine_;

    TDiskHealthCheckerPtr HealthChecker_;

    TLocationPerformanceCountersPtr PerformanceCounters_;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, LockedChunksLock_);
    THashSet<TChunkId> LockedChunkIds_;

    static EIOCategory ToIOCategory(const TWorkloadDescriptor& workloadDescriptor);

    void DecreasePendingIOSize(EIODirection direction, EIOCategory category, i64 delta);
    void UpdatePendingIOSize(EIODirection direction, EIOCategory category, i64 delta);

    void ValidateWritable();
    void InitializeCellId();
    void InitializeUuid();

    void UpdateMediumTag();

    void OnHealthCheckFailed(const TError& error);
    void MarkAsDisabled(const TError& error);

    void PopulateAlerts(std::vector<TError>* alerts);

    virtual i64 GetAdditionalSpace() const;

    virtual std::optional<TChunkDescriptor> RepairChunk(TChunkId chunkId) = 0;
    virtual std::vector<TString> GetChunkPartNames(TChunkId chunkId) const = 0;
};

DEFINE_REFCOUNTED_TYPE(TChunkLocation)

////////////////////////////////////////////////////////////////////////////////

class TStoreLocation
    : public TChunkLocation
{
public:
    struct TIOStatistics
    {
        i64 FilesystemReadRate = 0;
        i64 FilesystemWriteRate = 0;

        i64 DiskReadRate = 0;
        i64 DiskWriteRate = 0;
    };

    TStoreLocation(
        const TString& id,
        TStoreLocationConfigPtr config,
        NClusterNode::TClusterNodeDynamicConfigManagerPtr dynamicConfigManager,
        TChunkStorePtr chunkStore,
        TChunkContextPtr chunkContext,
        IChunkStoreHostPtr chunkStoreHost);

    ~TStoreLocation();

    //! Returns the location's config.
    const TStoreLocationConfigPtr& GetConfig() const;

    //! Returns Journal Manager associated with this location.
    const TJournalManagerPtr& GetJournalManager();

    //! Returns the space reserved for low watermark.
    //! Never throws.
    i64 GetLowWatermarkSpace() const;

    //! Returns max allowed write rate by device warranty.
    //! Never throws.
    i64 GetMaxWriteRateByDwpd() const;

    //! Checks whether the location is full.
    bool IsFull() const;

    //! Checks whether to location has enough space to contain file of size #size.
    bool HasEnoughSpace(i64 size) const;

    const NConcurrency::IThroughputThrottlerPtr& GetInThrottler(const TWorkloadDescriptor& descriptor) const;

    //! Removes a chunk permanently or moves it to the trash.
    void RemoveChunkFiles(TChunkId chunkId, bool force) override;

    //! Returns various IO related statistics.
    TIOStatistics GetIOStatistics() const;

    //! Returns |true| if the location accepts new writes.
    bool IsWritable() const;

private:
    const TStoreLocationConfigPtr Config_;

    const TJournalManagerPtr JournalManager_;
    const NConcurrency::TActionQueuePtr TrashCheckQueue_;

    mutable std::atomic<bool> Full_ = false;
    mutable std::atomic<bool> WritesDisabledDueToHighPendingReadSize_ = false;

    struct TTrashChunkEntry
    {
        TChunkId ChunkId;
        i64 DiskSpace;
    };

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, TrashMapSpinLock_);
    std::multimap<TInstant, TTrashChunkEntry> TrashMap_;
    std::atomic<i64> TrashDiskSpace_ = 0;
    const NConcurrency::TPeriodicExecutorPtr TrashCheckExecutor_;

    class TIOStatisticsProvider;
    const TIntrusivePtr<TIOStatisticsProvider> StatisticsProvider_;

    NConcurrency::IThroughputThrottlerPtr RepairInThrottler_;
    NConcurrency::IThroughputThrottlerPtr ReplicationInThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletCompactionAndPartitioningInThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletLoggingInThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletSnapshotInThrottler_;
    NConcurrency::IThroughputThrottlerPtr TabletStoreFlushInThrottler_;
    NConcurrency::IThroughputThrottlerPtr UnlimitedInThrottler_;

    TString GetTrashPath() const;
    TString GetTrashChunkPath(TChunkId chunkId) const;
    void RegisterTrashChunk(TChunkId chunkId);
    void OnCheckTrash();
    void CheckTrashTtl();
    void CheckTrashWatermark();
    void RemoveTrashFiles(const TTrashChunkEntry& entry);
    void MoveChunkFilesToTrash(TChunkId chunkId);

    i64 GetAdditionalSpace() const override;

    std::optional<TChunkDescriptor> RepairBlobChunk(TChunkId chunkId);
    std::optional<TChunkDescriptor> RepairJournalChunk(TChunkId chunkId);
    std::optional<TChunkDescriptor> RepairChunk(TChunkId chunkId) override;

    std::vector<TString> GetChunkPartNames(TChunkId chunkId) const override;
    bool ShouldSkipFileName(const TString& fileName) const override;

    void DoStart() override;
    std::vector<TChunkDescriptor> DoScan() override;
};

DEFINE_REFCOUNTED_TYPE(TStoreLocation)

////////////////////////////////////////////////////////////////////////////////

class TCacheLocation
    : public TChunkLocation
{
public:
    TCacheLocation(
        const TString& id,
        TCacheLocationConfigPtr config,
        NClusterNode::TClusterNodeDynamicConfigManagerPtr dynamicConfigManager,
        TChunkContextPtr chunkContext,
        IChunkStoreHostPtr chunkStoreHost);

    const NConcurrency::IThroughputThrottlerPtr& GetInThrottler() const;

private:
    const TCacheLocationConfigPtr Config_;

    const NConcurrency::IThroughputThrottlerPtr InThrottler_;

    std::optional<TChunkDescriptor> Repair(TChunkId chunkId, const TString& metaSuffix);
    std::optional<TChunkDescriptor> RepairChunk(TChunkId chunkId) override;

    std::vector<TString> GetChunkPartNames(TChunkId chunkId) const override;
};

DEFINE_REFCOUNTED_TYPE(TCacheLocation)

////////////////////////////////////////////////////////////////////////////////

class TPendingIOGuard
{
public:
    TPendingIOGuard() = default;
    TPendingIOGuard(TPendingIOGuard&& other) = default;
    ~TPendingIOGuard();

    void Release();

    TPendingIOGuard& operator = (TPendingIOGuard&& other) = default;

    explicit operator bool() const;
    i64 GetSize() const;

private:
    friend class TChunkLocation;

    TPendingIOGuard(
        EIODirection direction,
        EIOCategory category,
        i64 size,
        TChunkLocationPtr owner);

    EIODirection Direction_;
    EIOCategory Category_;
    i64 Size_ = 0;
    TChunkLocationPtr Owner_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode

#define LOCATION_INL_H_
#include "location-inl.h"
#undef LOCATION_INL_H_
