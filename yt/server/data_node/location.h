#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/server/misc/public.h>
#include <yt/server/misc/disk_location.h>

#include <yt/ytlib/chunk_client/chunk_info.pb.h>

#include <yt/core/actions/signal.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

#include <atomic>
#include <map>

namespace NYT {
namespace NDataNode {

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
    (Realtime)
);

class TLocation
    : public TDiskLocation
{
public:
    TLocation(
        ELocationType type,
        const Stroka& id,
        TStoreLocationConfigBasePtr config,
        NCellNode::TBootstrap* bootstrap);

    //! Returns the type.
    ELocationType GetType() const;

    //! Returns string id.
    const Stroka& GetId() const;

    //! Returns the profiler tagged with location id.
    const NProfiling::TProfiler& GetProfiler() const;

    //! Returns the root path of the location.
    Stroka GetPath() const;

    //! Returns the maximum number of bytes the chunks assigned to this location
    //! are allowed to use.
    i64 GetQuota() const;

    //! Returns an invoker for reading chunk data.
    IPrioritizedInvokerPtr GetDataReadInvoker();

    //! Returns an invoker for reading chunk meta.
    IPrioritizedInvokerPtr GetMetaReadInvoker();

    //! Returns an invoker for writing chunks.
    IInvokerPtr GetWritePoolInvoker();

    //! Scan the location directory removing orphaned files and returning the list of found chunks.
    /*!
     *  If the scan fails, the location becomes disabled, |Disabled| signal is raised, and an empty list is returned.
     */
    std::vector<TChunkDescriptor> Scan();

    //! Prepares the location to accept new writes.
    /*!
     *  Must be called when all locations are scanned and all existing chunks are registered.
     *  On failure, acts similarly to Scan.
     */
    void Start();

    //! Marks the location as disabled by attempting to create a lock file and terminates the process.
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

    //! Returns the load factor.
    double GetLoadFactor() const;

    //! Returns the number of bytes pending for disk IO.
    i64 GetPendingIOSize(
        EIODirection direction,
        const TWorkloadDescriptor& workloadDescriptor);

    //! Acquires a lock for the given number of bytes to be read or written.
    TPendingIOGuard IncreasePendingIOSize(
        EIODirection direction,
        const TWorkloadDescriptor& workloadDescriptor,
        i64 delta);

    //! Changes the number of currently active sessions by a given delta.
    void UpdateSessionCount(int delta);

    //! Changes the number of chunks by a given delta.
    void UpdateChunkCount(int delta);

    //! Returns the number of currently active sessions.
    int GetSessionCount() const;

    //! Returns the number of chunks.
    int GetChunkCount() const;

    //! Returns a full path for a primary chunk file.
    Stroka GetChunkPath(const TChunkId& chunkId) const;

    //! Permanently removes the files comprising a given chunk.
    void RemoveChunkFilesPermanently(const TChunkId& chunkId);

    //! Removes a chunk permanently or moves it to the trash (if available).
    virtual void RemoveChunkFiles(const TChunkId& chunkId, bool force);

    Stroka GetMediumName() const;

    int GetMediumIndex() const;
    void SetMediumIndex(int mediumIndex);

protected:
    NCellNode::TBootstrap* const Bootstrap_;


    static Stroka GetRelativeChunkPath(const TChunkId& chunkId);
    static void ForceHashDirectories(const Stroka& rootPath);

    virtual bool ShouldSkipFileName(const Stroka& fileName) const;

    virtual void DoStart();
    virtual std::vector<TChunkDescriptor> DoScan();

private:
    friend class TPendingIOGuard;

    const ELocationType Type_;
    const Stroka Id_;
    const TStoreLocationConfigBasePtr Config_;

    mutable i64 AvailableSpace_ = 0;
    i64 UsedSpace_ = 0;
    int SessionCount_ = 0;
    int ChunkCount_ = 0;

    const NConcurrency::TThreadPoolPtr DataReadThreadPool_;
    const IPrioritizedInvokerPtr DataReadInvoker_;

    const NConcurrency::TActionQueuePtr MetaReadQueue_;
    const IPrioritizedInvokerPtr MetaReadInvoker_;

    const NConcurrency::TThreadPoolPtr WriteThreadPool_;
    const IInvokerPtr WritePoolInvoker_;

    TDiskHealthCheckerPtr HealthChecker_;

    NProfiling::TProfiler Profiler_;
    //! Indexed by |(ioDirection, ioCategory)|.
    std::vector<NProfiling::TSimpleCounter> PendingIOSizeCounters_;

    TNullable<int> MediumIndex_;

    static EIOCategory ToIOCategory(const TWorkloadDescriptor& workloadDescriptor);
    NProfiling::TSimpleCounter& GetPendingIOSizeCounter(
        EIODirection direction,
        EIOCategory category);

    void DecreasePendingIOSize(EIODirection direction, EIOCategory category, i64 delta);
    void UpdatePendingIOSize(EIODirection direction, EIOCategory category, i64 delta);

    void ValidateLockFile();
    void ValidateWritable();

    void OnHealthCheckFailed(const TError& error);
    void MarkAsDisabled(const TError& error);

    virtual i64 GetAdditionalSpace() const;

    virtual TNullable<TChunkDescriptor> RepairChunk(const TChunkId& chunkId) = 0;

    virtual std::vector<Stroka> GetChunkPartNames(const TChunkId& chunkId) const = 0;

};

DEFINE_REFCOUNTED_TYPE(TLocation);

////////////////////////////////////////////////////////////////////////////////

class TStoreLocation
    : public TLocation
{
public:
    TStoreLocation(
        const Stroka& id,
        TStoreLocationConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

    //! Returns Journal Manager accociated with this location.
    TJournalManagerPtr GetJournalManager();

    //! Returns the space reserved for low watermark.
    //! Never throws.
    i64 GetLowWatermarkSpace() const;

    //! Checks whether the location is full.
    bool IsFull() const;

    //! Checks whether to location has enough space to contain file of size #size
    bool HasEnoughSpace(i64 size) const;

    //! Returns |true| if the location accepts new chunks of a given type.
    bool IsChunkTypeAccepted(NObjectClient::EObjectType chunkType);

    //! Removes a chunk permanently or moves it to the trash.
    virtual void RemoveChunkFiles(const TChunkId& chunkId, bool force) override;

private:
    const TStoreLocationConfigPtr Config_;

    const TJournalManagerPtr JournalManager_;
    const NConcurrency::TActionQueuePtr TrashCheckQueue_;

    struct TTrashChunkEntry
    {
        TChunkId ChunkId;
        i64 DiskSpace;
    };

    TSpinLock TrashMapSpinLock_;
    std::multimap<TInstant, TTrashChunkEntry> TrashMap_;
    i64 TrashDiskSpace_ = 0;
    const NConcurrency::TPeriodicExecutorPtr TrashCheckExecutor_;


    Stroka GetTrashPath() const;
    Stroka GetTrashChunkPath(const TChunkId& chunkId) const;
    void RegisterTrashChunk(const TChunkId& chunkId);
    void OnCheckTrash();
    void CheckTrashTtl();
    void CheckTrashWatermark();
    void RemoveTrashFiles(const TTrashChunkEntry& entry);
    void MoveChunkFilesToTrash(const TChunkId& chunkId);

    virtual i64 GetAdditionalSpace() const override;

    TNullable<TChunkDescriptor> RepairBlobChunk(const TChunkId& chunkId);
    TNullable<TChunkDescriptor> RepairJournalChunk(const TChunkId& chunkId);
    virtual TNullable<TChunkDescriptor> RepairChunk(const TChunkId& chunkId) override;

    virtual std::vector<Stroka> GetChunkPartNames(const TChunkId& chunkId) const override;
    virtual bool ShouldSkipFileName(const Stroka& fileName) const override;

    virtual void DoStart() override;
    virtual std::vector<TChunkDescriptor> DoScan() override;

};

DEFINE_REFCOUNTED_TYPE(TStoreLocation);

////////////////////////////////////////////////////////////////////////////////

class TCacheLocation
    : public TLocation
{
public:
    TCacheLocation(
        const Stroka& id,
        TCacheLocationConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

private:
    const TCacheLocationConfigPtr Config_;

    TNullable<TChunkDescriptor> Repair(const TChunkId& chunkId, const Stroka& metaSuffix);
    virtual TNullable<TChunkDescriptor> RepairChunk(const TChunkId& chunkId) override;

    virtual std::vector<Stroka> GetChunkPartNames(const TChunkId& chunkId) const override;
};

DEFINE_REFCOUNTED_TYPE(TCacheLocation);

////////////////////////////////////////////////////////////////////////////////

class TPendingIOGuard
{
public:
    TPendingIOGuard() = default;
    TPendingIOGuard(TPendingIOGuard&& other) = default;
    ~TPendingIOGuard();

    void Release();

    TPendingIOGuard& operator = (TPendingIOGuard&& other);

    explicit operator bool() const;
    i64 GetSize() const;

    friend void swap(TPendingIOGuard& lhs, TPendingIOGuard& rhs);

private:
    friend class TLocation;

    TPendingIOGuard(
        EIODirection direction,
        EIOCategory category,
        i64 size,
        TLocationPtr owner);

    EIODirection Direction_;
    EIOCategory Category_;
    i64 Size_ = 0;
    TLocationPtr Owner_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

#define LOCATION_INL_H_
#include "location-inl.h"
#undef LOCATION_INL_H_
