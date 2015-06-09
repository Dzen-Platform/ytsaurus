#pragma once

#include "public.h"

#include <core/concurrency/action_queue.h>
#include <core/concurrency/periodic_executor.h>

#include <core/actions/signal.h>

#include <core/logging/log.h>

#include <core/profiling/profiler.h>

#include <ytlib/chunk_client/chunk_info.pb.h>

#include <server/cell_node/public.h>

#include <atomic>
#include <map>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ELocationType,
    (Store)
    (Cache)
);

//! Describes a physical location of chunks.
class TLocation
    : public TRefCounted
{
public:
    TLocation(
        ELocationType type,
        const Stroka& id,
        TLocationConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

    ~TLocation();

    //! Returns the type.
    ELocationType GetType() const;

    //! Returns string id.
    const Stroka& GetId() const;

    //! Returns the location's configuration.
    TLocationConfigPtr GetConfig() const;

    //! Returns |true| if the location accepts new chunks of a given type.
    bool IsChunkTypeAccepted(NObjectClient::EObjectType chunkType);

    //! Returns the profiler tagged with location id.
    const NProfiling::TProfiler& GetProfiler();

    //! Scan the location directory removing orphaned files and returning the list of found chunks.
    /*!
     *  If the scan fails, the location becomes disabled, |Disabled| signal is raised, and an empty list is returned.
     */
    std::vector<TChunkDescriptor> Scan();

    //! Prepares the location to accept new writes.
    /*!
     *  Replays multiplexed journals.
     *  Must be called when all locations are scanned and all existing chunks are registered.
     *  On failure, acts similarly to Scan.
     */
    void Start();

    //! Updates #UsedSpace and #AvailalbleSpace
    void UpdateUsedSpace(i64 size);

    //! Updates #AvailalbleSpace with a system call and returns the result.
    //! Never throws.
    i64 GetAvailableSpace() const;

    //! Returns the total space on the disk drive where the location resides.
    //! Never throws.
    i64 GetTotalSpace() const;

    //! Returns the space reserved for low watermark.
    //! Never throws.
    i64 GetLowWatermarkSpace() const;

    //! Returns the number of bytes used at the location.
    /*!
     *  \note
     *  This may exceed #GetQuota.
     */
    i64 GetUsedSpace() const;

    //! Returns the maximum number of bytes the chunks assigned to this location
    //! are allowed to use.
    i64 GetQuota() const;

    //! Returns the root path of the location.
    Stroka GetPath() const;

    //! Returns the root trash path of the location.
    Stroka GetTrashPath() const;

    //! Returns the load factor.
    double GetLoadFactor() const;

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

    //! Returns a full path for a removed primary chunk file.
    Stroka GetTrashChunkPath(const TChunkId& chunkId) const;

    //! Checks whether the location is full.
    bool IsFull() const;

    //! Checks whether to location has enough space to contain file of size #size
    bool HasEnoughSpace(i64 size) const;

    //! Returns an invoker for reading chunk data.
    IPrioritizedInvokerPtr GetDataReadInvoker();

    //! Returns an invoker for reading chunk meta.
    IPrioritizedInvokerPtr GetMetaReadInvoker();

    //! Returns an invoker for writing chunks.
    IInvokerPtr GetWritePoolInvoker();

    //! Returns Journal Manager accociated with this location.
    TJournalManagerPtr GetJournalManager();

    //! Returns |true| iff the location is enabled.
    bool IsEnabled() const;

    //! Marks the location as disabled.
    void Disable(const TError& reason);

    //! Permanently removes the files comprising a given chunk.
    void RemoveChunkFiles(const TChunkId& chunkId);

    //! Moves the files comprising a given chunk into trash directory.
    void MoveChunkFilesToTrash(const TChunkId& chunkId);

    //! Raised when the location gets disabled.
    /*!
     *  Raised at most once in Control thread.
     */
    DEFINE_SIGNAL(void(const TError&), Disabled);

private:
    const ELocationType Type_;
    const Stroka Id_;
    const TLocationConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    NProfiling::TProfiler Profiler_;

    std::atomic<bool> Enabled_ = {false};

    mutable i64 AvailableSpace_ = 0;
    i64 UsedSpace_ = 0;
    int SessionCount_ = 0;
    int ChunkCount_ = 0;

    const NConcurrency::TFairShareActionQueuePtr ReadQueue_;
    const IPrioritizedInvokerPtr DataReadInvoker_;
    const IPrioritizedInvokerPtr MetaReadInvoker_;

    const NConcurrency::TThreadPoolPtr WriteThreadPool_;
    const IInvokerPtr WritePoolInvoker_;

    const TJournalManagerPtr JournalManager_;

    const TDiskHealthCheckerPtr HealthChecker_;

    struct TTrashChunkEntry
    {
        TChunkId ChunkId;
        i64 DiskSpace;
    };

    TSpinLock TrashMapSpinLock_;
    std::multimap<TInstant, TTrashChunkEntry> TrashMap_;
    i64 TrashDiskSpace_ = 0;
    const NConcurrency::TPeriodicExecutorPtr TrashCheckExecutor_;

    mutable NLogging::TLogger Logger;


    std::vector<TChunkDescriptor> DoScan();
    TNullable<TChunkDescriptor> RepairBlobChunk(const TChunkId& chunkId);
    TNullable<TChunkDescriptor> RepairJournalChunk(const TChunkId& chunkId);

    void DoStart();

    void OnHealthCheckFailed(const TError& error);
    void ScheduleDisable(const TError& reason);
    void DoDisable(const TError& reason);

    static Stroka GetRelativeChunkPath(const TChunkId& chunkId);
    std::vector<Stroka> GetChunkPartNames(const TChunkId& chunkId) const;

    void RegisterTrashChunk(const TChunkId& chunkId);
    void OnCheckTrash();
    void CheckTrashTtl();
    void CheckTrashWatermark();
    void RemoveTrashFiles(const TTrashChunkEntry& entry);

};

DEFINE_REFCOUNTED_TYPE(TLocation)

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

