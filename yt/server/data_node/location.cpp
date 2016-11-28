#include "location.h"
#include "private.h"
#include "blob_chunk.h"
#include "blob_reader_cache.h"
#include "config.h"
#include "journal_chunk.h"
#include "journal_dispatcher.h"
#include "journal_manager.h"
#include "master_connector.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/hydra/changelog.h>
#include <yt/server/hydra/private.h>

#include <yt/server/misc/disk_health_checker.h>
#include <yt/server/misc/private.h>

#include <yt/ytlib/chunk_client/format.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/misc/fs.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/concurrency/thread_pool.h>

namespace NYT {
namespace NDataNode {

using namespace NChunkClient;
using namespace NCellNode;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NHydra;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

// Others must not be able to list chunk store and chunk cache directories.
static const int ChunkFilesPermissions = 0751;
static const auto TrashCheckPeriod = TDuration::Seconds(10);

////////////////////////////////////////////////////////////////////////////////

TLocation::TLocation(
    ELocationType type,
    const Stroka& id,
    TStoreLocationConfigBasePtr config,
    TBootstrap* bootstrap)
    : TDiskLocation(config, id, DataNodeLogger)
    , Bootstrap_(bootstrap)
    , Type_(type)
    , Id_(id)
    , Config_(config)
    , DataReadThreadPool_(New<TThreadPool>(bootstrap->GetConfig()->DataNode->ReadThreadCount, Format("DataRead:%v", Id_)))
    , DataReadInvoker_(CreatePrioritizedInvoker(DataReadThreadPool_->GetInvoker()))
    , MetaReadQueue_(New<TActionQueue>(Format("MetaRead:%v", Id_)))
    , MetaReadInvoker_(CreatePrioritizedInvoker(MetaReadQueue_->GetInvoker()))
    , WriteThreadPool_(New<TThreadPool>(Bootstrap_->GetConfig()->DataNode->WriteThreadCount, Format("DataWrite:%v", Id_)))
    , WritePoolInvoker_(WriteThreadPool_->GetInvoker())
{
    auto* profileManager = NProfiling::TProfileManager::Get();
    NProfiling::TTagIdList tagIds{
        profileManager->RegisterTag("location_id", Id_),
        profileManager->RegisterTag("location_type", Type_)
    };
    Profiler_ = NProfiling::TProfiler(DataNodeProfiler.GetPathPrefix(), tagIds);

    HealthChecker_ = New<TDiskHealthChecker>(
        Bootstrap_->GetConfig()->DataNode->DiskHealthChecker,
        GetPath(),  
        GetWritePoolInvoker(),
        DataNodeLogger,
        Profiler_);

    PendingIOSizeCounters_.resize(
        TEnumTraits<EIODirection>::GetDomainSize() *
        TEnumTraits<EIOCategory>::GetDomainSize());
    for (auto direction : TEnumTraits<EIODirection>::GetDomainValues()) {
        for (auto category : TEnumTraits<EIOCategory>::GetDomainValues()) {
            auto& counter = GetPendingIOSizeCounter(direction, category);
            counter = NProfiling::TSimpleCounter(
                "/pending_data_size",
                {
                    profileManager->RegisterTag("direction", direction),
                    profileManager->RegisterTag("category", category)
                });
        }
    }
}

ELocationType TLocation::GetType() const
{
    return Type_;
}

const Stroka& TLocation::GetId() const
{
    return Id_;
}

const NProfiling::TProfiler& TLocation::GetProfiler() const
{
    return Profiler_;
}

Stroka TLocation::GetPath() const
{
    return Config_->Path;
}

i64 TLocation::GetQuota() const
{
    return Config_->Quota.Get(std::numeric_limits<i64>::max());
}

IPrioritizedInvokerPtr TLocation::GetDataReadInvoker()
{
    return DataReadInvoker_;
}

IPrioritizedInvokerPtr TLocation::GetMetaReadInvoker()
{
    return MetaReadInvoker_;
}

IInvokerPtr TLocation::GetWritePoolInvoker()
{
    return WritePoolInvoker_;
}

std::vector<TChunkDescriptor> TLocation::Scan()
{
    try {
        ValidateLockFile();
        ValidateMinimumSpace();
        ValidateWritable();
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Location disabled");
        MarkAsDisabled(ex);
        return std::vector<TChunkDescriptor>();
    }

    try {
        // Be optimistic and assume everything will be OK.
        // Also Disable requires Enabled_ to be true.
        Enabled_.store(true);
        return DoScan();
    } catch (const std::exception& ex) {
        Disable(TError("Location scan failed") << ex);
        Y_UNREACHABLE(); // Disable() exits the process.
    }
}

void TLocation::Start()
{
    if (!IsEnabled())
        return;

    try {
        DoStart();
    } catch (const std::exception& ex) {
        Disable(TError("Location start failed") << ex);
    }
}

void TLocation::Disable(const TError& reason)
{
    if (!Enabled_.exchange(false)) {
        // Save only once.
        Sleep(TDuration::Max());
    }

    LOG_ERROR(reason);

    // Save the reason in a file and exit.
    // Location will be disabled during the scan in the restart process.
    auto lockFilePath = NFS::CombinePaths(GetPath(), DisabledLockFileName);
    try {
        auto errorData = ConvertToYsonString(reason, NYson::EYsonFormat::Pretty).Data();
        TFile file(lockFilePath, CreateAlways | WrOnly | Seq | CloseOnExec);
        TFileOutput fileOutput(file);
        fileOutput << errorData;
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Error creating location lock file");
        // Exit anyway.
    }

    _exit(1);
}

void TLocation::UpdateUsedSpace(i64 size)
{
    if (!IsEnabled())
        return;

    UsedSpace_ += size;
    AvailableSpace_ -= size;
}

i64 TLocation::GetUsedSpace() const
{
    return UsedSpace_;
}

i64 TLocation::GetAvailableSpace() const
{
    if (!IsEnabled()) {
        return 0;
    }

    auto path = GetPath();

    try {
        auto statistics = NFS::GetDiskSpaceStatistics(path);
        AvailableSpace_ = statistics.AvailableSpace + GetAdditionalSpace();
    } catch (const std::exception& ex) {
        auto error = TError("Failed to compute available space")
            << ex;
        const_cast<TLocation*>(this)->Disable(error);
        Y_UNREACHABLE(); // Disable() exits the process.
    }

    i64 remainingQuota = std::max(static_cast<i64>(0), GetQuota() - GetUsedSpace());
    AvailableSpace_ = std::min(AvailableSpace_, remainingQuota);

    return AvailableSpace_;
}

double TLocation::GetLoadFactor() const
{
    i64 used = GetUsedSpace();
    i64 quota = GetQuota();
    return used >= quota ? 1.0 : (double) used / quota;
}

i64 TLocation::GetPendingIOSize(
    EIODirection direction,
    const TWorkloadDescriptor& workloadDescriptor)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto category = ToIOCategory(workloadDescriptor);
    return GetPendingIOSizeCounter(direction, category).Current.load();
}

TPendingIOGuard TLocation::IncreasePendingIOSize(
    EIODirection direction,
    const TWorkloadDescriptor& workloadDescriptor,
    i64 delta)
{
    VERIFY_THREAD_AFFINITY_ANY();

    Y_ASSERT(delta >= 0);
    auto category = ToIOCategory(workloadDescriptor);
    UpdatePendingIOSize(direction, category, delta);
    return TPendingIOGuard(direction, category, delta, this);
}

EIOCategory TLocation::ToIOCategory(const TWorkloadDescriptor& workloadDescriptor)
{
    switch (workloadDescriptor.Category) {
        case EWorkloadCategory::Idle:
        case EWorkloadCategory::SystemReplication:
        case EWorkloadCategory::SystemTabletCompaction:
        case EWorkloadCategory::SystemTabletPartitioning:
        case EWorkloadCategory::SystemTabletPreload:
        case EWorkloadCategory::SystemArtifactCacheDownload:
        case EWorkloadCategory::UserBatch:
            return EIOCategory::Batch;

        case EWorkloadCategory::UserRealtime:
        case EWorkloadCategory::SystemRealtime:
            return EIOCategory::Realtime;

        case EWorkloadCategory::SystemRepair:
            return EIOCategory::Repair;

        default:
            // Graceful fallback for possible future extensions of categories.
            return EIOCategory::Batch;
    }
}

NProfiling::TSimpleCounter& TLocation::GetPendingIOSizeCounter(
    EIODirection direction,
    EIOCategory category)
{
    int index =
        static_cast<int>(direction) +
        TEnumTraits<EIODirection>::GetDomainSize() * static_cast<int>(category);
    return PendingIOSizeCounters_[index];
}

void TLocation::DecreasePendingIOSize(
    EIODirection direction,
    EIOCategory category,
    i64 delta)
{
    VERIFY_THREAD_AFFINITY_ANY();

    UpdatePendingIOSize(direction, category, -delta);
}

void TLocation::UpdatePendingIOSize(
    EIODirection direction,
    EIOCategory category,
    i64 delta)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto& counter = GetPendingIOSizeCounter(direction, category);
    i64 result = Profiler_.Increment(counter, delta);
    LOG_TRACE("Pending IO size updated (Direction: %v, Category: %v, PendingSize: %v, Delta: %v)",
        direction,
        category,
        result,
        delta);
}

void TLocation::UpdateSessionCount(int delta)
{
    if (!IsEnabled())
        return;

    SessionCount_ += delta;
}

int TLocation::GetSessionCount() const
{
    return SessionCount_;
}

void TLocation::UpdateChunkCount(int delta)
{
    if (!IsEnabled())
        return;

    ChunkCount_ += delta;
}

int TLocation::GetChunkCount() const
{
    return ChunkCount_;
}

Stroka TLocation::GetChunkPath(const TChunkId& chunkId) const
{
    return NFS::CombinePaths(GetPath(), GetRelativeChunkPath(chunkId));
}

void TLocation::RemoveChunkFilesPermanently(const TChunkId& chunkId)
{
    try {
        LOG_DEBUG("Started removing chunk files (ChunkId: %v)", chunkId);

        auto partNames = GetChunkPartNames(chunkId);
        auto directory = NFS::GetDirectoryName(GetChunkPath(chunkId));

        for (const auto& name : partNames) {
            auto fileName = NFS::CombinePaths(directory, name);
            if (NFS::Exists(fileName)) {
                NFS::Remove(fileName);
            }
        }

        LOG_DEBUG("Finished removing chunk files (ChunkId: %v)", chunkId);
    } catch (const std::exception& ex) {
        auto error = TError(
            NChunkClient::EErrorCode::IOError,
            "Error removing chunk %v",
            chunkId)
            << ex;
        Disable(error);
        Y_UNREACHABLE(); // Disable() exits the process.
    }
}

void TLocation::RemoveChunkFiles(const TChunkId& chunkId, bool force)
{
    Y_UNUSED(force);
    RemoveChunkFilesPermanently(chunkId);
}

Stroka TLocation::GetRelativeChunkPath(const TChunkId& chunkId)
{
    int hashByte = chunkId.Parts32[0] & 0xff;
    return NFS::CombinePaths(Format("%02x", hashByte), ToString(chunkId));
}

void TLocation::ForceHashDirectories(const Stroka& rootPath)
{
    for (int hashByte = 0; hashByte <= 0xff; ++hashByte) {
        auto hashDirectory = Format("%02x", hashByte);
        NFS::ForcePath(NFS::CombinePaths(rootPath, hashDirectory), ChunkFilesPermissions);
    }
}

void TLocation::ValidateLockFile()
{
    LOG_INFO("Checking lock file");

    auto lockFilePath = NFS::CombinePaths(GetPath(), DisabledLockFileName);
    if (!NFS::Exists(lockFilePath)) {
        return;
    }

    TFile file(lockFilePath, OpenExisting | RdOnly | Seq | CloseOnExec);
    TBufferedFileInput fileInput(file);

    auto errorData = fileInput.ReadAll();
    if (errorData.Empty()) {
        THROW_ERROR_EXCEPTION("Empty lock file found");
    }

    TError error;
    try {
        error = ConvertTo<TError>(TYsonString(errorData));
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing lock file contents")
            << ex;
    }
    THROW_ERROR error;
}

void TLocation::ValidateWritable()
{
    NFS::ForcePath(GetPath(), ChunkFilesPermissions);

    // Run first health check before to sort out read-only drives.
    HealthChecker_->RunCheck()
        .Get()
        .ThrowOnError();
}

void TLocation::OnHealthCheckFailed(const TError& error)
{
    Disable(error);
    Y_UNREACHABLE(); // Disable() exits the process.
}

void TLocation::MarkAsDisabled(const TError& error)
{
    auto alert = TError("Chunk location at %v is disabled", GetPath())
        << error;
    auto masterConnector = Bootstrap_->GetMasterConnector();
    masterConnector->RegisterAlert(alert);

    Enabled_.store(false);

    AvailableSpace_ = 0;
    UsedSpace_ = 0;
    SessionCount_ = 0;
    ChunkCount_ = 0;
}

i64 TLocation::GetAdditionalSpace() const
{
    return 0;
}

bool TLocation::ShouldSkipFileName(const Stroka& fileName) const
{
    // Skip cell_id file.
    if (fileName == CellIdFileName)
        return true;

    return false;
}

std::vector<TChunkDescriptor> TLocation::DoScan()
{
    LOG_INFO("Scanning storage location");

    NFS::CleanTempFiles(GetPath());
    ForceHashDirectories(GetPath());

    yhash_set<TChunkId> chunkIds;
    {
        // Enumerate files under the location's directory.
        // Note that these also include trash files but the latter are explicitly skipped.
        auto fileNames = NFS::EnumerateFiles(GetPath(), std::numeric_limits<int>::max());
        for (const auto& fileName : fileNames) {
            if (ShouldSkipFileName(fileName))
                continue;

            TChunkId chunkId;
            auto bareFileName = NFS::GetFileNameWithoutExtension(fileName);
            if (!TChunkId::FromString(bareFileName, &chunkId)) {
                LOG_ERROR("Unrecognized file %v in location directory", fileName);
                continue;
            }

            chunkIds.insert(chunkId);
        }
    }

    // Construct the list of chunk descriptors.
    // Also "repair" half-alive chunks (e.g. those having some of their essential parts missing)
    // by moving them into trash.
    std::vector<TChunkDescriptor> descriptors;
    for (const auto& chunkId : chunkIds) {
        auto maybeDescriptor = RepairChunk(chunkId);
        if (maybeDescriptor) {
            descriptors.push_back(*maybeDescriptor);
        }
    }

    LOG_INFO("Done, %v chunks found", descriptors.size());

    return descriptors;
}

void TLocation::DoStart()
{
    auto cellIdPath = NFS::CombinePaths(GetPath(), CellIdFileName);
    if (NFS::Exists(cellIdPath)) {
        TFileInput cellIdFile(cellIdPath);
        auto cellIdString = cellIdFile.ReadAll();
        TCellId cellId;
        if (!TCellId::FromString(cellIdString, &cellId)) {
            THROW_ERROR_EXCEPTION("Failed to parse cell id %Qv",
                cellIdString);
        }
        if (cellId != Bootstrap_->GetCellId()) {
            THROW_ERROR_EXCEPTION("Wrong cell id: expected %v, found %v",
                Bootstrap_->GetCellId(),
                cellId);
        }
    } else {
        LOG_INFO("Cell id file is not found, creating");
        TFile file(cellIdPath, CreateAlways | WrOnly | Seq | CloseOnExec);
        TFileOutput cellIdFile(file);
        cellIdFile.Write(ToString(Bootstrap_->GetCellId()));
    }

    HealthChecker_->SubscribeFailed(BIND(&TLocation::OnHealthCheckFailed, Unretained(this)));
    HealthChecker_->Start();
}

const Stroka& TLocation::GetMediumName() const
{
    return Config_->MediumName;
}

int TLocation::GetMediumIndex() const
{
    YCHECK(MediumIndex_ != InvalidMediumIndex);

    return MediumIndex_;
}

void TLocation::SetMediumIndex(int mediumIndex)
{
    MediumIndex_ = mediumIndex;
}

////////////////////////////////////////////////////////////////////////////////

TStoreLocation::TStoreLocation(
    const Stroka& id,
    TStoreLocationConfigPtr config,
    TBootstrap* bootstrap)
    : TLocation(
        ELocationType::Store,
        id,
        config,
        bootstrap)
    , Config_(config)
    , JournalManager_(New<TJournalManager>(
        bootstrap->GetConfig()->DataNode,
        this,
        bootstrap))
    , TrashCheckQueue_(New<TActionQueue>(Format("Trash:%v", id)))
    , TrashCheckExecutor_(New<TPeriodicExecutor>(
        TrashCheckQueue_->GetInvoker(),
        BIND(&TStoreLocation::OnCheckTrash, MakeWeak(this)),
        TrashCheckPeriod,
        EPeriodicExecutorMode::Automatic))
    , RepairInThrottler_(CreateReconfigurableThroughputThrottler(config->RepairInThrottler))
    , ReplicationInThrottler_(CreateReconfigurableThroughputThrottler(config->ReplicationInThrottler))
{ }

TJournalManagerPtr TStoreLocation::GetJournalManager()
{
    return JournalManager_;
}

i64 TStoreLocation::GetLowWatermarkSpace() const
{
    return Config_->LowWatermark;
}

bool TStoreLocation::IsFull() const
{
    return GetAvailableSpace() < Config_->LowWatermark;
}

bool TStoreLocation::HasEnoughSpace(i64 size) const
{
    return GetAvailableSpace() - size >= Config_->HighWatermark;
}

bool TStoreLocation::IsChunkTypeAccepted(EObjectType chunkType)
{
    if (!IsEnabled()) {
        return false;
    }

    if (IsFull()) {
        return false;
    }

    switch (chunkType) {
        case EObjectType::Chunk:
        case EObjectType::ErasureChunk:
            return Config_->EnableBlobs;

        case EObjectType::JournalChunk:
            return Config_->EnableJournals;

        default:
            Y_UNREACHABLE();
    }
}

IThroughputThrottlerPtr TStoreLocation::GetInThrottler(const TWorkloadDescriptor& descriptor) const
{
   switch (descriptor.Category) {
        case EWorkloadCategory::SystemRepair:
            return RepairInThrottler_;

        case EWorkloadCategory::SystemReplication:
            return ReplicationInThrottler_;

        default:
            return GetUnlimitedThrottler();
    }
}

void TStoreLocation::RemoveChunkFiles(const TChunkId& chunkId, bool force)
{
    if (force) {
        RemoveChunkFilesPermanently(chunkId);
    } else {
        MoveChunkFilesToTrash(chunkId);
    }
}

Stroka TStoreLocation::GetTrashPath() const
{
    return NFS::CombinePaths(GetPath(), TrashDirectory);
}

Stroka TStoreLocation::GetTrashChunkPath(const TChunkId& chunkId) const
{
    return NFS::CombinePaths(GetTrashPath(), GetRelativeChunkPath(chunkId));
}

void TStoreLocation::RegisterTrashChunk(const TChunkId& chunkId)
{
    auto timestamp = TInstant::Zero();
    i64 diskSpace = 0;
    auto partNames = GetChunkPartNames(chunkId);
    for (const auto& name : partNames) {
        auto directory = NFS::GetDirectoryName(GetTrashChunkPath(chunkId));
        auto fileName = NFS::CombinePaths(directory, name);
        if (NFS::Exists(fileName)) {
            auto statistics = NFS::GetFileStatistics(fileName);
            timestamp = std::max(timestamp, statistics.ModificationTime);
            diskSpace += statistics.Size;
        }
    }

    {
        TGuard<TSpinLock> guard(TrashMapSpinLock_);
        TrashMap_.insert(std::make_pair(timestamp, TTrashChunkEntry{chunkId, diskSpace}));
        TrashDiskSpace_ += diskSpace;
    }

    LOG_DEBUG("Trash chunk registered (ChunkId: %v, Timestamp: %v, DiskSpace: %v)",
        chunkId,
        timestamp,
        diskSpace);
}

void TStoreLocation::OnCheckTrash()
{
    if (!IsEnabled())
        return;

    try {
        CheckTrashTtl();
        CheckTrashWatermark();
    } catch (const std::exception& ex) {
        auto error = TError("Error checking trash")
            << ex;
        Disable(error);
        Y_UNREACHABLE(); // Disable() exits the process.
    }
}

void TStoreLocation::CheckTrashTtl()
{
    auto deadline = TInstant::Now() - Config_->MaxTrashTtl;
    while (true) {
        TTrashChunkEntry entry;
        {
            TGuard<TSpinLock> guard(TrashMapSpinLock_);
            if (TrashMap_.empty())
                break;
            auto it = TrashMap_.begin();
            if (it->first >= deadline)
                break;
            entry = it->second;
            TrashMap_.erase(it);
            TrashDiskSpace_ -= entry.DiskSpace;
        }
        RemoveTrashFiles(entry);
    }
}

void TStoreLocation::CheckTrashWatermark()
{
    i64 availableSpace;
    auto beginCleanup = [&] () {
        TGuard<TSpinLock> guard(TrashMapSpinLock_);
        // NB: Available space includes trash disk space.
        availableSpace = GetAvailableSpace() - TrashDiskSpace_;
        return availableSpace < Config_->TrashCleanupWatermark && !TrashMap_.empty();
    };

    if (!beginCleanup())
        return;

    LOG_INFO("Low available disk space, starting trash cleanup (AvailableSpace: %v)",
        availableSpace);

    while (beginCleanup()) {
        while (true) {
            TTrashChunkEntry entry;
            {
                TGuard<TSpinLock> guard(TrashMapSpinLock_);
                if (TrashMap_.empty())
                    break;
                auto it = TrashMap_.begin();
                entry = it->second;
                TrashMap_.erase(it);
                TrashDiskSpace_ -= entry.DiskSpace;
            }
            RemoveTrashFiles(entry);
            availableSpace += entry.DiskSpace;
        }
    }

    LOG_INFO("Finished trash cleanup (AvailableSpace: %v)",
        availableSpace);
}

void TStoreLocation::RemoveTrashFiles(const TTrashChunkEntry& entry)
{
    auto partNames = GetChunkPartNames(entry.ChunkId);
    for (const auto& name : partNames) {
        auto directory = NFS::GetDirectoryName(GetTrashChunkPath(entry.ChunkId));
        auto fileName = NFS::CombinePaths(directory, name);
        if (NFS::Exists(fileName)) {
            NFS::Remove(fileName);
        }
    }

    LOG_DEBUG("Trash chunk removed (ChunkId: %v, DiskSpace: %v)",
        entry.ChunkId,
        entry.DiskSpace);
}

void TStoreLocation::MoveChunkFilesToTrash(const TChunkId& chunkId)
{
    try {
        LOG_DEBUG("Started moving chunk files to trash (ChunkId: %v)", chunkId);

        auto partNames = GetChunkPartNames(chunkId);
        auto directory = NFS::GetDirectoryName(GetChunkPath(chunkId));
        auto trashDirectory = NFS::GetDirectoryName(GetTrashChunkPath(chunkId));

        for (const auto& name : partNames) {
            auto srcFileName = NFS::CombinePaths(directory, name);
            auto dstFileName = NFS::CombinePaths(trashDirectory, name);
            if (NFS::Exists(srcFileName)) {
                NFS::Replace(srcFileName, dstFileName);
                NFS::Touch(dstFileName);
            }
        }

        LOG_DEBUG("Finished moving chunk files to trash (ChunkId: %v)", chunkId);

        RegisterTrashChunk(chunkId);
    } catch (const std::exception& ex) {
        auto error = TError(
            NChunkClient::EErrorCode::IOError,
            "Error moving chunk %v to trash",
            chunkId)
            << ex;
        Disable(error);
        Y_UNREACHABLE(); // Disable() exits the process.
    }
}

i64 TStoreLocation::GetAdditionalSpace() const
{
    // NB: Unguarded access to TrashDiskSpace_ seems OK.
    return TrashDiskSpace_;
}

TNullable<TChunkDescriptor> TStoreLocation::RepairBlobChunk(const TChunkId& chunkId)
{
    auto fileName = GetChunkPath(chunkId);
    auto trashFileName = GetTrashChunkPath(chunkId);

    auto dataFileName = fileName;
    auto metaFileName = fileName + ChunkMetaSuffix;

    auto trashDataFileName = trashFileName;
    auto trashMetaFileName = trashFileName + ChunkMetaSuffix;

    bool hasData = NFS::Exists(dataFileName);
    bool hasMeta = NFS::Exists(metaFileName);

    if (hasMeta && hasData) {
        i64 dataSize = NFS::GetFileStatistics(dataFileName).Size;
        i64 metaSize = NFS::GetFileStatistics(metaFileName).Size;
        if (metaSize > 0) {
            TChunkDescriptor descriptor;
            descriptor.Id = chunkId;
            descriptor.DiskSpace = dataSize + metaSize;
            return descriptor;
        }
        // EXT4 specific thing.
        // See https://bugs.launchpad.net/ubuntu/+source/linux/+bug/317781
        LOG_WARNING("Chunk meta file %v is empty, removing chunk files",
            metaFileName);
        NFS::Remove(dataFileName);
        NFS::Remove(metaFileName);
    } else if (!hasMeta && hasData) {
        LOG_WARNING("Chunk meta file %v is missing, moving data file %v to trash",
            metaFileName,
            dataFileName);
        NFS::Replace(dataFileName, trashDataFileName);
    } else if (!hasData && hasMeta) {
        LOG_WARNING("Chunk data file %v is missing, moving meta file %v to trash",
            dataFileName,
            metaFileName);
        NFS::Replace(metaFileName, trashMetaFileName);
    }
    return Null;
}

TNullable<TChunkDescriptor> TStoreLocation::RepairJournalChunk(const TChunkId& chunkId)
{
    auto fileName = GetChunkPath(chunkId);
    auto trashFileName = GetTrashChunkPath(chunkId);

    auto dataFileName = fileName;
    auto indexFileName = fileName + "." + ChangelogIndexExtension;

    auto trashIndexFileName = trashFileName + "." + ChangelogIndexExtension;

    bool hasData = NFS::Exists(dataFileName);
    bool hasIndex = NFS::Exists(indexFileName);

    if (hasData) {
        auto dispatcher = Bootstrap_->GetJournalDispatcher();
        // NB: This also creates the index file, if missing.
        auto changelog = dispatcher->OpenChangelog(this, chunkId)
            .Get()
            .ValueOrThrow();
        TChunkDescriptor descriptor;
        descriptor.Id = chunkId;
        descriptor.DiskSpace = changelog->GetDataSize();
        descriptor.RowCount = changelog->GetRecordCount();
        descriptor.Sealed = dispatcher->IsChangelogSealed(this, chunkId)
            .Get()
            .ValueOrThrow();
        return descriptor;

    } else if (!hasData && hasIndex) {
        LOG_WARNING("Journal data file %v is missing, moving index file %v to trash",
            dataFileName,
            indexFileName);
        NFS::Replace(indexFileName, trashIndexFileName);
    }

    return Null;
}

TNullable<TChunkDescriptor> TStoreLocation::RepairChunk(const TChunkId& chunkId)
{
    TNullable<TChunkDescriptor> maybeDescriptor;
    auto chunkType = TypeFromId(DecodeChunkId(chunkId).Id);
    switch (chunkType) {
        case EObjectType::Chunk:
        case EObjectType::ErasureChunk:
            maybeDescriptor = RepairBlobChunk(chunkId);
            break;

        case EObjectType::JournalChunk:
            maybeDescriptor = RepairJournalChunk(chunkId);
            break;

        default:
            LOG_WARNING("Invalid type %Qlv of chunk %v, skipped",
                chunkType,
                chunkId);
            break;
    }
    return maybeDescriptor;
}

std::vector<Stroka> TStoreLocation::GetChunkPartNames(const TChunkId& chunkId) const
{
    auto primaryName = ToString(chunkId);
    switch (TypeFromId(DecodeChunkId(chunkId).Id)) {
        case EObjectType::Chunk:
        case EObjectType::ErasureChunk:
            return {
                primaryName,
                primaryName + ChunkMetaSuffix
            };

        case EObjectType::JournalChunk:
            return {
                primaryName,
                primaryName + "." + ChangelogIndexExtension,
                primaryName + "." + SealedFlagExtension
            };

        default:
            Y_UNREACHABLE();
    }
}

bool TStoreLocation::ShouldSkipFileName(const Stroka& fileName) const
{
    if (TLocation::ShouldSkipFileName(fileName)) {
        return true;
    }

    // Skip trash directory.
    if (fileName.has_prefix(TrashDirectory + LOCSLASH_S))
        return true;

    // Skip multiplexed directory.
    if (fileName.has_prefix(MultiplexedDirectory + LOCSLASH_S))
        return true;

    return false;
}

std::vector<TChunkDescriptor> TStoreLocation::DoScan()
{
    auto result = TLocation::DoScan();

    LOG_INFO("Scanning storage trash");

    ForceHashDirectories(GetTrashPath());

    yhash_set<TChunkId> trashChunkIds;
    {
        // Enumerate files under the location's trash directory.
        // Note that some of them might have just been moved there during repair.
        auto fileNames = NFS::EnumerateFiles(GetTrashPath(), std::numeric_limits<int>::max());

        for (const auto& fileName : fileNames) {
            TChunkId chunkId;
            auto bareFileName = NFS::GetFileNameWithoutExtension(fileName);
            if (!TChunkId::FromString(bareFileName, &chunkId)) {
                LOG_ERROR("Unrecognized file %v in location trash directory", fileName);
                continue;
            }
            trashChunkIds.insert(chunkId);
        }

        for (const auto& chunkId : trashChunkIds) {
            RegisterTrashChunk(chunkId);
        }
    }

    LOG_INFO("Done, %v trash chunks found", trashChunkIds.size());

    return result;
}

void TStoreLocation::DoStart()
{
    TLocation::DoStart();

    JournalManager_->Initialize();

    TrashCheckExecutor_->Start();
}

////////////////////////////////////////////////////////////////////////////////

TCacheLocation::TCacheLocation(
    const Stroka& id,
    TCacheLocationConfigPtr config,
    TBootstrap* bootstrap)
    : TLocation(
        ELocationType::Cache,
        id,
        config,
        bootstrap)
    , Config_(config)
    , InThrottler_(CreateReconfigurableThroughputThrottler(config->InThrottler))
{ }

IThroughputThrottlerPtr TCacheLocation::GetInThrottler() const
{
    return InThrottler_;
}

TNullable<TChunkDescriptor> TCacheLocation::Repair(
    const TChunkId& chunkId,
    const Stroka& metaSuffix)
{
    auto fileName = GetChunkPath(chunkId);

    auto dataFileName = fileName;
    auto metaFileName = fileName + metaSuffix;

    bool hasData = NFS::Exists(dataFileName);
    bool hasMeta = NFS::Exists(metaFileName);

    if (hasMeta && hasData) {
        i64 dataSize = NFS::GetFileStatistics(dataFileName).Size;
        i64 metaSize = NFS::GetFileStatistics(metaFileName).Size;
        if (metaSize > 0) {
            TChunkDescriptor descriptor;
            descriptor.Id = chunkId;
            descriptor.DiskSpace = dataSize + metaSize;
            return descriptor;
        }
        LOG_WARNING("Chunk meta file %v is empty, removing chunk files",
            metaFileName);
    } else if (hasData && !hasMeta) {
        LOG_WARNING("Chunk meta file %v is missing, removing data file %v",
            metaFileName,
            dataFileName);
    } else if (!hasData && hasMeta) {
        LOG_WARNING("Chunk data file %v is missing, removing meta file %v",
            dataFileName,
            metaFileName);
    }

    if (hasData) {
        NFS::Remove(dataFileName);
    }
    if (hasMeta) {
        NFS::Remove(metaFileName);
    }

    return Null;
}

TNullable<TChunkDescriptor> TCacheLocation::RepairChunk(const TChunkId& chunkId)
{
    TNullable<TChunkDescriptor> maybeDescriptor;
    auto chunkType = TypeFromId(DecodeChunkId(chunkId).Id);
    switch (chunkType) {
        case EObjectType::Chunk:
            maybeDescriptor = Repair(chunkId, ChunkMetaSuffix);
            break;

        case EObjectType::Artifact:
            maybeDescriptor = Repair(chunkId, ArtifactMetaSuffix);
            break;

        default:
            LOG_WARNING("Invalid type %Qlv of chunk %v, skipped",
                chunkType,
                chunkId);
            break;
    }
    return maybeDescriptor;
}

std::vector<Stroka> TCacheLocation::GetChunkPartNames(const TChunkId& chunkId) const
{
    auto primaryName = ToString(chunkId);
    switch (TypeFromId(DecodeChunkId(chunkId).Id)) {
        case EObjectType::Chunk:
            return {
                primaryName,
                primaryName + ChunkMetaSuffix
            };

        case EObjectType::Artifact:
            return {
                primaryName,
                primaryName + ArtifactMetaSuffix
            };

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

TPendingIOGuard::TPendingIOGuard(
    EIODirection direction,
    EIOCategory category,
    i64 size,
    TLocationPtr owner)
    : Direction_(direction)
    , Category_(category)
    , Size_(size)
    , Owner_(owner)
{ }

TPendingIOGuard& TPendingIOGuard::operator=(TPendingIOGuard&& other)
{
    swap(*this, other);
    return *this;
}

TPendingIOGuard::~TPendingIOGuard()
{
    Release();
}

void TPendingIOGuard::Release()
{
    if (Owner_) {
        Owner_->DecreasePendingIOSize(Direction_, Category_, Size_);
        Owner_.Reset();
    }
}

TPendingIOGuard::operator bool() const
{
    return Owner_.operator bool();
}

i64 TPendingIOGuard::GetSize() const
{
    return Size_;
}

void swap(TPendingIOGuard& lhs, TPendingIOGuard& rhs)
{
    using std::swap;
    swap(lhs.Direction_, rhs.Direction_);
    swap(lhs.Category_, rhs.Category_);
    swap(lhs.Size_, rhs.Size_);
    swap(lhs.Owner_, rhs.Owner_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
