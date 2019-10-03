#ifdef __linux__

#include "volume_manager.h"
#include "disk_location.h"

#include "artifact.h"
#include "chunk.h"
#include "chunk_cache.h"
#include "master_connector.h"
#include "private.h"

#include <yt/server/node/data_node/volume.pb.h>

#include <yt/server/node/cell_node/bootstrap.h>
#include <yt/server/node/cell_node/config.h>

#include <yt/server/lib/containers/porto_executor.h>

#include <yt/server/lib/misc/disk_health_checker.h>
#include <yt/server/lib/misc/private.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/async_cache.h>
#include <yt/core/misc/checksum.h>
#include <yt/core/misc/fs.h>
#include <yt/core/misc/finally.h>
#include <yt/core/misc/proc.h>

#include <yt/core/tools/tools.h>

namespace NYT::NDataNode {

using namespace NConcurrency;
using namespace NContainers;
using namespace NCellNode;
using namespace NTools;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

const auto& Logger = DataNodeLogger;

static const TString StorageSuffix = "storage";
static const TString MountSuffix = "mount";

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TPortoVolumeManager);

////////////////////////////////////////////////////////////////////////////////

using TLayerId = TGuid;
using TVolumeId = TGuid;

//! Used for layer and for volume meta files.
struct TLayerMetaHeader
{
    ui64 Signature = ExpectedSignature;

    //! Version of layer meta format. Update every time layer meta version is updated.
    ui64 Version = ExpectedVersion;

    ui64 MetaChecksum;

    static constexpr ui64 ExpectedSignature = 0xbe17d73ce7ff9ea6ull; // YTLMH001
    static constexpr ui64 ExpectedVersion = 1;
};

struct TLayerMeta
    : public NProto::TLayerMeta
{
    TString Path;
    TLayerId Id;
};

////////////////////////////////////////////////////////////////////////////////

struct TVolumeKey
{
    const std::vector<TArtifactKey> LayerKeys;

    explicit TVolumeKey(std::vector<TArtifactKey> layerKeys)
        : LayerKeys(std::move(layerKeys))
    { }

    // Hasher.
    operator size_t() const
    {
        size_t result = 0;
        for (const auto& artifactKey : LayerKeys) {
            HashCombine(result, size_t(artifactKey));
        }
        return result;
    }

    bool operator == (const TVolumeKey& other) const
    {
        return LayerKeys == other.LayerKeys;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TVolumeMeta
    : public NProto::TVolumeMeta
{
    TVolumeId Id;
    TString StoragePath;
    TString MountPath;
};

////////////////////////////////////////////////////////////////////////////////

static const TString VolumesName = "volumes";
static const TString LayersName = "porto_layers";
static const TString LayersMetaName = "layers_meta";
static const TString VolumesMetaName = "volumes_meta";

class TLayerLocation
    : public TDiskLocation
{
public:
    TLayerLocation(
        const TLayerLocationConfigPtr& locationConfig,
        const TDiskHealthCheckerConfigPtr healthCheckerConfig,
        IPortoExecutorPtr volumeExecutor,
        IPortoExecutorPtr layerExecutor,
        const TString& id)
        : TDiskLocation(locationConfig, id, DataNodeLogger)
        , Config_(locationConfig)
        , VolumeExecutor_(std::move(volumeExecutor))
        , LayerExecutor_(std::move(layerExecutor))
        , LocationQueue_(New<TActionQueue>(id))
        , VolumesPath_(NFS::CombinePaths(Config_->Path, VolumesName))
        , VolumesMetaPath_(NFS::CombinePaths(Config_->Path, VolumesMetaName))
        , LayersPath_(NFS::CombinePaths(Config_->Path, LayersName))
        , LayersMetaPath_(NFS::CombinePaths(Config_->Path, LayersMetaName))
    {
        HealthChecker_ = New<TDiskHealthChecker>(
            healthCheckerConfig,
            locationConfig->Path,
            LocationQueue_->GetInvoker(),
            Logger);

        // If true, location is placed on a YT-specific drive, binded into container from dom0 host,
        // so it has absolute path relative to dom0 root.
        // Otherwise, location is placed inside a persistent volume, and should be treated differently.
        // More details here: PORTO-460.
        PlacePath_ = (Config_->LocationIsAbsolute ? "" : "//") + Config_->Path;

        try {
            NFS::MakeDirRecursive(Config_->Path, 0755);
            WaitFor(HealthChecker_->RunCheck())
                .ThrowOnError();

            // Volumes are not expected to be used since all jobs must be dead by now.
            auto volumes = WaitFor(VolumeExecutor_->ListVolumes())
                .ValueOrThrow();

            std::vector<TFuture<void>> unlinkFutures;
            for (const auto& volume : volumes) {
                if (volume.Path.StartsWith(VolumesPath_)) {
                    unlinkFutures.push_back(VolumeExecutor_->UnlinkVolume(volume.Path, "self"));
                }
            }
            WaitFor(Combine(unlinkFutures))
                .ThrowOnError();

            RunTool<TRemoveDirAsRootTool>(VolumesPath_);
            RunTool<TRemoveDirAsRootTool>(VolumesMetaPath_);

            NFS::MakeDirRecursive(VolumesPath_, 0755);
            NFS::MakeDirRecursive(LayersPath_, 0755);
            NFS::MakeDirRecursive(VolumesMetaPath_, 0755);
            NFS::MakeDirRecursive(LayersMetaPath_, 0755);
            NFS::MakeDirRecursive(LayersMetaPath_, 0755);
            // This is requires to use directory as place.
            NFS::MakeDirRecursive(NFS::CombinePaths(Config_->Path, "porto_volumes"), 0755);
            NFS::MakeDirRecursive(NFS::CombinePaths(Config_->Path, "porto_storage"), 0755);

            ValidateMinimumSpace();

            LoadLayers();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Failed to initialize layer location %v",
                Config_->Path)
                << ex;
        }

        HealthChecker_->SubscribeFailed(BIND(&TLayerLocation::Disable, MakeWeak(this))
            .Via(LocationQueue_->GetInvoker()));
        HealthChecker_->Start();
        Enabled_ = true;
    }

    TFuture<TLayerMeta> ImportLayer(const TArtifactKey& artifactKey, const TString& archivePath, TGuid tag)
    {
        return BIND(&TLayerLocation::DoImportLayer, MakeStrong(this), artifactKey, archivePath, tag)
            .AsyncVia(LocationQueue_->GetInvoker())
            .Run();
    }

    void RemoveLayer(const TLayerId& layerId)
    {
        BIND(&TLayerLocation::DoRemoveLayer, MakeStrong(this), layerId)
            .Via(LocationQueue_->GetInvoker())
            .Run();
    }

    TFuture<TVolumeMeta> CreateVolume(const std::vector<TLayerMeta>& layers)
    {
        return BIND(&TLayerLocation::DoCreateVolume, MakeStrong(this), layers)
            .AsyncVia(LocationQueue_->GetInvoker())
            .Run();
    }

    void RemoveVolume(const TVolumeId& volumeId)
    {
        BIND(&TLayerLocation::DoRemoveVolume, MakeStrong(this), volumeId)
            .Via(LocationQueue_->GetInvoker())
            .Run();
    }

    std::vector<TLayerMeta> GetAllLayers() const
    {
        std::vector<TLayerMeta> layers;

        auto guard = Guard(SpinLock);
        for (const auto& pair : Layers_) {
            layers.push_back(pair.second);
        }
        return layers;
    }

    void Disable(const TError& error)
    {
        if (!Enabled_.exchange(false)) {
            Sleep(TDuration::Max());
        }

        // Save the reason in a file and exit.
        // Location will be disabled during the scan in the restart process.
        auto lockFilePath = NFS::CombinePaths(Config_->Path, DisabledLockFileName);
        try {
            auto errorData = ConvertToYsonString(error, NYson::EYsonFormat::Pretty).GetData();
            TFile file(lockFilePath, CreateAlways | WrOnly | Seq | CloseOnExec);
            TFileOutput fileOutput(file);
            fileOutput << errorData;
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Error creating location lock file");
            // Exit anyway.
        }

        YT_LOG_ERROR("Volume manager disabled; terminating");
        NLogging::TLogManager::Get()->Shutdown();
        _exit(1);
    }

    int GetLayerCount() const
    {
        auto guard = Guard(SpinLock);
        return Layers_.size();
    }

    int GetVolumeCount() const
    {
        auto guard = Guard(SpinLock);
        return Volumes_.size();
    }

    bool IsFull()
    {
        return GetAvailableSpace() < Config_->LowWatermark;
    }

    bool IsLayerImportInProgress() const
    {
        return LayerImportsInProgress_.load() > 0;
    }

    i64 GetCapacity()
    {
        return std::max<i64>(0, UsedSpace_ + GetAvailableSpace() - Config_->LowWatermark);
    }

    i64 GetAvailableSpace()
    {
        if (!IsEnabled()) {
            return 0;
        }

        const auto& path = Config_->Path;

        try {
            auto statistics = NFS::GetDiskSpaceStatistics(path);
            AvailableSpace_ = statistics.AvailableSpace;
        } catch (const std::exception& ex) {
            auto error = TError("Failed to compute available space")
                << ex;
            Disable(error);
            YT_ABORT(); // Disable() exits the process.
        }

        i64 remainingQuota = std::max(static_cast<i64>(0), GetQuota() - UsedSpace_);
        AvailableSpace_ = std::min(AvailableSpace_, remainingQuota);

        return AvailableSpace_;
    }

private:
    const TLayerLocationConfigPtr Config_;
    const IPortoExecutorPtr VolumeExecutor_;
    const IPortoExecutorPtr LayerExecutor_;

    const TActionQueuePtr LocationQueue_ ;
    TDiskHealthCheckerPtr HealthChecker_;

    TString PlacePath_;

    TSpinLock SpinLock;
    const TString VolumesPath_;
    const TString VolumesMetaPath_;
    const TString LayersPath_;
    const TString LayersMetaPath_;

    std::atomic<int> LayerImportsInProgress_ = { 0 };

    THashMap<TLayerId, TLayerMeta> Layers_;
    THashMap<TVolumeId, TVolumeMeta> Volumes_;

    mutable i64 AvailableSpace_ = 0;
    i64 UsedSpace_ = 0;

    TString GetLayerPath(const TLayerId& id) const
    {
        return NFS::CombinePaths(LayersPath_, ToString(id));
    }

    TString GetLayerMetaPath(const TLayerId& id) const
    {
        return NFS::CombinePaths(LayersMetaPath_, ToString(id)) + ".meta";
    }

    TString GetVolumePath(const TVolumeId& id) const
    {
        return NFS::CombinePaths(VolumesPath_, ToString(id));
    }

    TString GetVolumeMetaPath(const TVolumeId& id) const
    {
        return NFS::CombinePaths(VolumesMetaPath_, ToString(id)) + ".meta";
    }

    void ValidateEnabled() const
    {
        if (!IsEnabled()) {
            THROW_ERROR_EXCEPTION(
                //EErrorCode::SlotLocationDisabled,
                "Layer location at %v is disabled",
                Config_->Path);
        }
    }

    THashSet<TLayerId> LoadLayerIds()
    {
        auto fileNames = NFS::EnumerateFiles(LayersMetaPath_);
        THashSet<TGuid> fileIds;
        for (const auto& fileName : fileNames) {
            if (fileName.EndsWith(NFS::TempFileSuffix)) {
                YT_LOG_DEBUG("Remove temporary file (Path: %v)",
                    fileName);
                NFS::Remove(fileName);
                continue;
            }

            auto nameWithoutExtension = NFS::GetFileNameWithoutExtension(fileName);
            TGuid id;
            if (!TGuid::FromString(nameWithoutExtension, &id)) {
                YT_LOG_ERROR("Unrecognized file in layer location directory (Path: %v)",
                    fileName);
                continue;
            }

            fileIds.insert(id);
        }

        THashSet<TGuid> confirmedIds;
        auto layerNames = WaitFor(LayerExecutor_->ListLayers(PlacePath_))
            .ValueOrThrow();

        for (const auto& layerName : layerNames) {
            TGuid id;
            if (!TGuid::FromString(layerName, &id)) {
                YT_LOG_ERROR("Unrecognized layer name in layer location directory (LayerName: %v)",
                    layerName);
                continue;
            }

            if (!fileIds.contains(id)) {
                YT_LOG_DEBUG("Remove directory without a corresponding meta file (LayerName: %v)",
                    layerName);
                WaitFor(LayerExecutor_->RemoveLayer(layerName, PlacePath_))
                    .ThrowOnError();
                continue;
            }

            YT_VERIFY(confirmedIds.insert(id).second);
            YT_VERIFY(fileIds.erase(id) == 1);
        }

        for (const auto& id : fileIds) {
            auto path = GetLayerMetaPath(id);
            YT_LOG_DEBUG("Remove layer meta file with no matching layer (Path: %v)",
                path);
            NFS::Remove(path);
        }

        return confirmedIds;
    }

    void LoadLayers()
    {
        auto ids = LoadLayerIds();

        for (const auto& id : ids) {
            auto metaFileName = GetLayerMetaPath(id);

            TFile metaFile(
                metaFileName,
                OpenExisting | RdOnly | Seq | CloseOnExec);

            if (metaFile.GetLength() < sizeof (TLayerMetaHeader)) {
                THROW_ERROR_EXCEPTION(
                    NChunkClient::EErrorCode::IncorrectLayerFileSize,
                    "Layer meta file %v is too short: at least %v bytes expected",
                    metaFileName,
                    sizeof (TLayerMetaHeader));
            }

            auto metaFileBlob = TSharedMutableRef::Allocate(metaFile.GetLength());

            NFS::ExpectIOErrors([&] () {
                TFileInput metaFileInput(metaFile);
                metaFileInput.Read(metaFileBlob.Begin(), metaFile.GetLength());
            });

            const auto* metaHeader = reinterpret_cast<const TLayerMetaHeader*>(metaFileBlob.Begin());
            if (metaHeader->Signature != TLayerMetaHeader::ExpectedSignature) {
                THROW_ERROR_EXCEPTION("Incorrect layer header signature %x in layer meta file %v",
                    metaHeader->Signature,
                    metaFileName);
            }

            auto metaBlob = TRef(metaFileBlob.Begin() + sizeof(TLayerMetaHeader), metaFileBlob.End());
            if (metaHeader->MetaChecksum != GetChecksum(metaBlob)) {
                THROW_ERROR_EXCEPTION("Incorrect layer meta checksum in layer meta file %v",
                    metaFileName);
            }

            NProto::TLayerMeta protoMeta;
            if (!TryDeserializeProtoWithEnvelope(&protoMeta, metaBlob)) {
                THROW_ERROR_EXCEPTION("Failed to parse chunk meta file %v",
                    metaFileName);
            }

            TLayerMeta meta;
            meta.MergeFrom(protoMeta);
            meta.Id = id;
            meta.Path = GetLayerPath(id);

            UsedSpace_ += meta.size();

            auto guard = Guard(SpinLock);
            YT_VERIFY(Layers_.insert(std::make_pair(id, meta)).second);
        }
    }

    i64 GetQuota() const
    {
        return Config_->Quota.value_or(std::numeric_limits<i64>::max());
    }

    TLayerMeta DoImportLayer(const TArtifactKey& artifactKey, const TString& archivePath, TGuid tag)
    {
        ValidateEnabled();

        auto id = TLayerId::Create();
        LayerImportsInProgress_.fetch_add(1);

        auto finally = Finally([&]{
            LayerImportsInProgress_.fetch_add(-1);
        });
        try {
            YT_LOG_DEBUG("Ensure that cached layer archive is not in use (LayerId: %v, ArchivePath: %v, Tag: %v)",
                id,
                archivePath,
                tag);

            {
                // Take exclusive lock in blocking fashion to ensure that no
                // forked process is holding an open descriptor to the source file.
                TFile file(archivePath, RdOnly | CloseOnExec);
                file.Flock(LOCK_EX);
            }

            YT_LOG_DEBUG("Create new directory for layer (LayerId: %v, Tag: %v)",
                id,
                tag);

            auto layerDirectory = GetLayerPath(id);

            try {
                YT_LOG_DEBUG("Unpack layer (Path: %v, Tag: %v)",
                    layerDirectory,
                    tag);
                WaitFor(LayerExecutor_->ImportLayer(archivePath, ToString(id), PlacePath_))
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                YT_LOG_ERROR(ex, "Layer unpacking failed (LayerId: %v, ArchivePath: %v, Tag: %v)",
                    id,
                    archivePath,
                    tag);
                THROW_ERROR_EXCEPTION(EErrorCode::LayerUnpackingFailed, "Layer unpacking failed")
                    << ex;
            }

            auto layerSize = RunTool<TGetDirectorySizeAsRootTool>(layerDirectory);

            YT_LOG_DEBUG("Calculated layer size (LayerId: %v, Size: %v, Tag: %v)",
                id,
                layerSize,
                tag);

            TLayerMeta layerMeta;
            layerMeta.Path = layerDirectory;
            layerMeta.Id = id;
            layerMeta.mutable_artifact_key()->MergeFrom(artifactKey);
            layerMeta.set_size(layerSize);
            ToProto(layerMeta.mutable_id(), id);

            auto metaBlob = SerializeProtoToRefWithEnvelope(layerMeta);

            TLayerMetaHeader header;
            header.MetaChecksum = GetChecksum(metaBlob);

            auto layerMetaFileName = GetLayerMetaPath(id);
            auto temporaryLayerMetaFileName = layerMetaFileName + NFS::TempFileSuffix;

            TFile metaFile(
                temporaryLayerMetaFileName,
                CreateAlways | WrOnly | Seq | CloseOnExec);
            metaFile.Write(&header, sizeof(header));
            metaFile.Write(metaBlob.Begin(), metaBlob.Size());
            metaFile.Close();

            NFS::Rename(temporaryLayerMetaFileName, layerMetaFileName);

            AvailableSpace_ -= layerSize;
            UsedSpace_ += layerSize;

            {
                auto guard = Guard(SpinLock);
                Layers_[id] = layerMeta;
            }

            YT_LOG_INFO("Finished importing layer (LayerId: %v, LayerPath: %v, UsedSpace: %v, AvailableSpace: %v, Tag: %v)",
                id,
                layerDirectory,
                UsedSpace_,
                AvailableSpace_,
                tag);

            return layerMeta;
        } catch (const std::exception& ex) {
            auto error = TError("Failed to import layer %v", id)
                << ex;

            auto innerError = TError(ex);
            if (innerError.GetCode() == EErrorCode::LayerUnpackingFailed) {
                THROW_ERROR error;
            }

            Disable(error);
            YT_ABORT();
        }
    }

    void DoRemoveLayer(const TLayerId& layerId)
    {
        ValidateEnabled();

        auto layerPath = GetLayerPath(layerId);
        auto layerMetaPath = GetLayerMetaPath(layerId);

        try {
            YT_LOG_INFO("Removing layer (LayerId: %v, LayerPath: %v)",
                layerId,
                layerPath);
            LayerExecutor_->RemoveLayer(ToString(layerId), PlacePath_);
            NFS::Remove(layerMetaPath);
        } catch (const std::exception& ex) {
            auto error = TError("Failed to remove layer %v",
                layerId)
                << ex;
            Disable(error);
            YT_ABORT();
        }

        i64 layerSize = -1;

        {
            auto guard = Guard(SpinLock);
            layerSize = Layers_[layerId].size();
            Layers_.erase(layerId);
        }

        UsedSpace_ -= layerSize;
        AvailableSpace_ += layerSize;
    }

    TVolumeMeta DoCreateVolume(const std::vector<TLayerMeta>& layers)
    {
        ValidateEnabled();

        auto id = TVolumeId::Create();
        auto volumePath = GetVolumePath(id);

        auto storagePath = NFS::CombinePaths(volumePath, StorageSuffix);
        auto mountPath = NFS::CombinePaths(volumePath, MountSuffix);

        try {
            YT_LOG_DEBUG("Creating volume (VolumeId: %v)",
                id);

            NFS::MakeDirRecursive(storagePath, 0755);
            NFS::MakeDirRecursive(mountPath, 0755);

            std::map<TString, TString> parameters;
            parameters["backend"] = "overlay";
            parameters["storage"] = storagePath;

            TStringBuilder builder;
            for (const auto& layer : layers) {
                if (builder.GetLength() > 0) {
                    builder.AppendChar(';');
                }
                builder.AppendString(layer.Path);
            }

            parameters["layers"] = builder.Flush();

            auto volumeId = WaitFor(VolumeExecutor_->CreateVolume(mountPath, parameters))
                .ValueOrThrow();

            YT_VERIFY(volumeId.Path == mountPath);

            YT_LOG_INFO("Volume created (VolumeId: %v, VolumeMountPath: %v)",
                id,
                mountPath);

            TVolumeMeta volumeMeta;
            for (const auto& layer : layers) {
                volumeMeta.add_layer_artifact_keys()->MergeFrom(layer.artifact_key());
                volumeMeta.add_layer_paths(layer.Path);
            }
            ToProto(volumeMeta.mutable_id(), id);
            volumeMeta.StoragePath = storagePath;
            volumeMeta.MountPath = mountPath;
            volumeMeta.Id = id;

            auto metaBlob = SerializeProtoToRefWithEnvelope(volumeMeta);

            TLayerMetaHeader header;
            header.MetaChecksum = GetChecksum(metaBlob);

            auto volumeMetaFileName = GetVolumeMetaPath(id);
            auto tempVolumeMetaFileName = volumeMetaFileName + NFS::TempFileSuffix;

            {
                auto metaFile = std::make_unique<TFile>(
                    tempVolumeMetaFileName ,
                    CreateAlways | WrOnly | Seq | CloseOnExec);
                metaFile->Write(&header, sizeof(header));
                metaFile->Write(metaBlob.Begin(), metaBlob.Size());
                metaFile->Close();
            }

            NFS::Rename(tempVolumeMetaFileName, volumeMetaFileName);

            YT_LOG_INFO("Volume meta created (VolumeId: %v, MetaFileName: %v)",
                id,
                volumeMetaFileName);

            auto guard = Guard(SpinLock);
            YT_VERIFY(Volumes_.insert(std::make_pair(id, volumeMeta)).second);

            return volumeMeta;
        } catch (const std::exception& ex) {
            auto error = TError("Failed to create volume %v", id)
                << ex;
            Disable(error);
            YT_ABORT();
        }
    }

    void DoRemoveVolume(const TVolumeId& volumeId)
    {
        ValidateEnabled();

        {
            auto guard = Guard(SpinLock);
            YT_VERIFY(Volumes_.contains(volumeId));
        }

        auto volumePath = GetVolumePath(volumeId);
        auto mountPath = NFS::CombinePaths(volumePath, MountSuffix);
        auto volumeMetaPath = GetVolumeMetaPath(volumeId);

        try {
            YT_LOG_DEBUG("Removing volume (VolumeId: %v)",
                volumeId);

            WaitFor(VolumeExecutor_->UnlinkVolume(mountPath, "self"))
                .ThrowOnError();

            YT_LOG_DEBUG("Volume unlinked (VolumeId: %v)",
                volumeId);

            RunTool<TRemoveDirAsRootTool>(volumePath);
            NFS::Remove(volumeMetaPath);

            YT_LOG_INFO("Volume directory and meta removed (VolumeId: %v, VolumePath: %v, VolumeMetaPath: %v)",
                volumeId,
                volumePath,
                volumeMetaPath);

            auto guard = Guard(SpinLock);
            YT_VERIFY(Volumes_.erase(volumeId) == 1);
        } catch (const std::exception& ex) {
            auto error = TError("Failed to remove volume %v", volumeId)
                << ex;
            Disable(error);
            YT_ABORT();
        }
    }
};

DEFINE_REFCOUNTED_TYPE(TLayerLocation)
DECLARE_REFCOUNTED_CLASS(TLayerLocation)

////////////////////////////////////////////////////////////////////////////////

i64 GetCacheCapacity(const std::vector<TLayerLocationPtr>& layerLocations)
{
    i64 result = 0;
    for (const auto& location : layerLocations) {
        result += location->GetCapacity();
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

TLayerLocationPtr DoPickLocation(
    const std::vector<TLayerLocationPtr> locations,
    std::function<bool(const TLayerLocationPtr&, const TLayerLocationPtr&)> isBetter)
{
    TLayerLocationPtr location;
    for (const auto& candidate : locations) {
        if (!candidate->IsEnabled()) {
            continue;
        }

        if (!location) {
            location = candidate;
            continue;
        }

        if (!candidate->IsFull() && isBetter(candidate, location)) {
            location = candidate;
        }
    }

    if (!location) {
        THROW_ERROR_EXCEPTION("Failed to get layer location; all locations are disabled");
    }

    return location;
}

////////////////////////////////////////////////////////////////////////////////

class TLayer
    : public TAsyncCacheValueBase<TArtifactKey, TLayer>
{
public:
    TLayer(const TLayerMeta& layerMeta, const TArtifactKey& artifactKey, const TLayerLocationPtr& layerLocation)
        : TAsyncCacheValueBase<TArtifactKey, TLayer>(artifactKey)
        , LayerMeta_(layerMeta)
        , Location_(layerLocation)
    { }

    ~TLayer()
    {
        YT_LOG_INFO("Layer is destroyed (LayerId: %v, LayerPath: %v)",
            LayerMeta_.Id,
            LayerMeta_.Path);
        Location_->RemoveLayer(LayerMeta_.Id);
    }

    const TString& GetPath() const
    {
        return LayerMeta_.Path;
    }

    void SubscribeEvicted(TCallback<void()> callback)
    {
        Evicted_.ToFuture()
           .Subscribe(BIND([=] (const TError& error) {
                YT_VERIFY(error.IsOK());
                callback.Run();
            }));
    }

    i64 GetSize() const
    {
        return LayerMeta_.size();
    }

    void OnEvicted()
    {
        YT_LOG_DEBUG("Layer is evicted (LayerId: %v)",
            LayerMeta_.Id);
        Evicted_.Set();
    }

    const TLayerMeta& GetMeta() const
    {
        return LayerMeta_;
    }

private:
    const TLayerMeta LayerMeta_;

    const TLayerLocationPtr Location_;

    TPromise<void> Evicted_ = NewPromise<void>();
};

DEFINE_REFCOUNTED_TYPE(TLayer)
DECLARE_REFCOUNTED_CLASS(TLayer)

////////////////////////////////////////////////////////////////////////////////

class TLayerCache
    : public TAsyncSlruCacheBase<TArtifactKey, TLayer>
{
public:
    TLayerCache(
        const TVolumeManagerConfigPtr& config,
        std::vector<TLayerLocationPtr> layerLocations,
        TBootstrap* bootstrap)
        : TAsyncSlruCacheBase(
            New<TSlruCacheConfig>(GetCacheCapacity(layerLocations) * config->CacheCapacityFraction),
            DataNodeProfiler.AppendPath("/layer_cache"))
        , Bootstrap_(bootstrap)
        , LayerLocations_(std::move(layerLocations))
        , Semaphore_(New<TAsyncSemaphore>(config->LayerImportConcurrency))
    {
        for (const auto& location : LayerLocations_) {
            for (const auto& layerMeta : location->GetAllLayers()) {
                TArtifactKey key;
                key.MergeFrom(layerMeta.artifact_key());
                auto layer = New<TLayer>(layerMeta, key, location);
                auto cookie = BeginInsert(layer->GetKey());
                if (cookie.IsActive()) {
                    cookie.EndInsert(layer);
                }
            }
        }
    }

    TFuture<TLayerPtr> PrepareLayer(const TArtifactKey& artifactKey, TGuid tag)
    {
        auto cookie = BeginInsert(artifactKey);
        auto value = cookie.GetValue();
        if (cookie.IsActive()) {
            auto& chunkCache = Bootstrap_->GetChunkCache();

            YT_LOG_DEBUG("Start loading layer into cache (Tag: %v, ArtifactKey: %v)",
                tag,
                artifactKey);

            TArtifactDownloadOptions downloadOptions;
            downloadOptions.NodeDirectory = Bootstrap_->GetNodeDirectory();
            chunkCache->DownloadArtifact(artifactKey, downloadOptions)
                .Subscribe(BIND([=, this_ = MakeStrong(this), cookie_ = std::move(cookie)] (const TErrorOr<IChunkPtr>& artifactChunkOrError) mutable {
                    try {
                        YT_LOG_DEBUG("Layer artifact loaded, starting import (Tag: %v, Error: %v, ArtifactKey: %v)",
                            tag,
                            artifactChunkOrError,
                            artifactKey);

                        // NB: ensure that artifact stays alive until the end of layer import.
                        const auto& artifactChunk = artifactChunkOrError.ValueOrThrow();

                        // NB(psushin): we limit number of concurrently imported layers, since this is heavy operation 
                        // which may delay light operations performed in the same IO thread pool inside porto daemon.
                        // PORTO-518
                        TAsyncSemaphoreGuard guard;
                        while (!(guard = TAsyncSemaphoreGuard::TryAcquire(Semaphore_))) {
                            WaitFor(Semaphore_->GetReadyEvent())
                                .ThrowOnError();
                        }

                        auto location = this_->PickLocation();
                        auto layerMeta = WaitFor(location->ImportLayer(artifactKey, artifactChunk->GetFileName(), tag))
                            .ValueOrThrow();

                        auto layer = New<TLayer>(layerMeta, artifactKey, location);
                        cookie_.EndInsert(layer);
                    } catch (const std::exception& ex) {
                        cookie_.Cancel(ex);
                    }
                })
                // We must pass this action through invoker to avoid synchronous execution.
                // WaitFor calls inside this action can ruin context-switch-free handlers inside TJob.
                .Via(GetCurrentInvoker()));
        } else {
            YT_LOG_DEBUG("Layer is already being loaded into cache (Tag: %v, ArtifactKey: %v)",
                tag,
                artifactKey);
        }

        return value;
    }

    void Touch(const TLayerPtr& layer)
    {
        Find(layer->GetKey());
    }

private:
    TBootstrap* const Bootstrap_;
    const std::vector<TLayerLocationPtr> LayerLocations_;

    TAsyncSemaphorePtr Semaphore_;

    virtual bool IsResurrectionSupported() const override
    {
        return false;
    }

    virtual i64 GetWeight(const TLayerPtr& layer) const override
    {
        return layer->GetSize();
    }

    virtual void OnRemoved(const TLayerPtr& layer) override
    {
        layer->OnEvicted();
    }

    TLayerLocationPtr PickLocation() const
    {
        return DoPickLocation(LayerLocations_, [] (const TLayerLocationPtr& candidate, const TLayerLocationPtr& current) {
            if (!candidate->IsLayerImportInProgress() && current->IsLayerImportInProgress()) {
                // Always prefer candidate which is not doing import right now.
                return true;
            } else if (candidate->IsLayerImportInProgress() && !current->IsLayerImportInProgress()) {
                return false;
            }

            return candidate->GetAvailableSpace() > current->GetAvailableSpace();
        });
    }
};

DECLARE_REFCOUNTED_CLASS(TLayerCache)
DEFINE_REFCOUNTED_TYPE(TLayerCache)

////////////////////////////////////////////////////////////////////////////////

class TVolumeState
    : public TRefCounted
{
public:
    TVolumeState(
        const TVolumeMeta& meta,
        TPortoVolumeManagerPtr owner,
        TLayerLocationPtr location,
        const std::vector<TLayerPtr>& layers)
        : VolumeMeta_(meta)
        , Owner_(std::move(owner))
        , Location_(std::move(location))
        , Layers_(layers)
    {
        auto callback = BIND(&TVolumeState::OnLayerEvicted, MakeWeak(this));
        // NB: We need a copy of layers vector here since OnLayerEvicted may be invoked in-place and cause Layers_ change.
        for (const auto& layer : layers) {
            layer->SubscribeEvicted(callback);
        }
    }

    ~TVolumeState()
    {
        YT_LOG_INFO("Destroying volume (VolumeId: %v)",
            VolumeMeta_.Id);

        Location_->RemoveVolume(VolumeMeta_.Id);
    }

    bool TryAcquireLock()
    {
        auto guard = Guard(SpinLock_);
        if (Evicted_) {
            return false;
        }

        ActiveCount_ += 1;
        return true;
    }

    void ReleaseLock()
    {
        auto guard = Guard(SpinLock_);
        ActiveCount_ -= 1;

        if (Evicted_ && ActiveCount_ == 0) {
            ReleaseLayers(std::move(guard));
        }
    }

    const TString& GetPath() const
    {
        return VolumeMeta_.MountPath;
    }

    const std::vector<TLayerPtr>& GetLayers() const
    {
        return Layers_;
    }

private:
    const TVolumeMeta VolumeMeta_;
    const TPortoVolumeManagerPtr Owner_;
    const TLayerLocationPtr Location_;

    TSpinLock SpinLock_;
    std::vector<TLayerPtr> Layers_;

    int ActiveCount_= 1;
    bool Evicted_ = false;

    void OnLayerEvicted()
    {
        // Do not consider this volume being cached any more.
        std::vector<TArtifactKey> layerKeys;
        for (const auto& layerKey : VolumeMeta_.layer_artifact_keys()) {
            TArtifactKey key;
            key.MergeFrom(layerKey);
            layerKeys.push_back(key);
        }

        auto volumeKey = TVolumeKey(std::move(layerKeys));

        auto guard = Guard(SpinLock_);
        Evicted_ = true;
        if (ActiveCount_ == 0) {
            ReleaseLayers(std::move(guard));
        }
    }

    void ReleaseLayers(TGuard<TSpinLock>&& guard)
    {
        std::vector<TLayerPtr> layers;
        std::swap(layers, Layers_);
        guard.Release();
    }
};

DECLARE_REFCOUNTED_CLASS(TVolumeState)
DEFINE_REFCOUNTED_TYPE(TVolumeState)

////////////////////////////////////////////////////////////////////////////////

class TLayeredVolume
    : public IVolume
{
public:
    TLayeredVolume(TVolumeStatePtr volumeState, bool isLocked)
        : VolumeState_(std::move(volumeState))
    {
        if (!isLocked && !VolumeState_->TryAcquireLock()) {
            THROW_ERROR_EXCEPTION("Failed to lock volume state, volume is waiting to be destroyed");
        }
    }

    ~TLayeredVolume()
    {
        VolumeState_->ReleaseLock();
    }

    virtual const TString& GetPath() const override
    {
        return VolumeState_->GetPath();
    }

private:
    const TVolumeStatePtr VolumeState_;
};

////////////////////////////////////////////////////////////////////////////////

class TPortoVolumeManager
    : public IVolumeManager
{
public:
    TPortoVolumeManager(const TVolumeManagerConfigPtr& config, TBootstrap* bootstrap)
    {
        // Create locations.
        for (int index = 0; index < config->LayerLocations.size(); ++index) {
            const auto& locationConfig = config->LayerLocations[index];
            auto id = Format("layers%v", index);

            try {
                auto location = New<TLayerLocation>(
                    locationConfig,
                    bootstrap->GetConfig()->DataNode->DiskHealthChecker,
                    CreatePortoExecutor(Format("volume_%v", index), config->PortoRetryTimeout, config->PortoPollPeriod),
                    CreatePortoExecutor(Format("layer_%v", index), config->PortoRetryTimeout, config->PortoPollPeriod),
                    id);
                Locations_.push_back(location);
            } catch (const std::exception& ex) {
                auto error = TError("Layer location at %v is disabled", locationConfig->Path)
                    << ex;
                YT_LOG_WARNING(error);
                auto masterConnector = bootstrap->GetMasterConnector();
                masterConnector->RegisterAlert(error);
            }
        }

        LayerCache_ = New<TLayerCache>(config, Locations_, bootstrap);
    }

    virtual TFuture<IVolumePtr> PrepareVolume(const std::vector<TArtifactKey>& layers) override
    {
        YT_VERIFY(!layers.empty());

        auto volumeKey = TVolumeKey(layers);
        auto tag = TGuid::Create();

        auto createVolume = [=] (bool isLocked, const TVolumeStatePtr& volumeState) {
            for (const auto& layer : volumeState->GetLayers()) {
                LayerCache_->Touch(layer);
            }

            YT_LOG_DEBUG("Creating new layered volume (Tag: %v, Path: %v)",
                tag,
                volumeState->GetPath());

            return New<TLayeredVolume>(volumeState, isLocked);
        };

        auto promise = NewPromise<TVolumeStatePtr>();
        promise.OnCanceled(BIND([=] () mutable {
            promise.TrySet(TError(NYT::EErrorCode::Canceled, "Root volume preparation was canceled")
                << TErrorAttribute("preparation_tag", tag));
        }));

        std::vector<TFuture<TLayerPtr>> layerFutures;
        layerFutures.reserve(layers.size());
        for (const auto& layerKey : layers) {
            layerFutures.push_back(LayerCache_->PrepareLayer(layerKey, tag));
        }

        // ToDo(psushin): choose proper invoker.
        // Avoid sync calls to WaitFor, to please job preparation context switch guards.
        Combine(layerFutures)
            .Subscribe(BIND(
                &TPortoVolumeManager::OnLayersPrepared,
                MakeStrong(this),
                promise,
                volumeKey,
                tag)
            .Via(GetCurrentInvoker()));

        return promise.ToFuture()
            .Apply(BIND(createVolume, true))
            .As<IVolumePtr>();
    }

private:
    std::vector<TLayerLocationPtr> Locations_;

    TLayerCachePtr LayerCache_;

    std::atomic<bool> Enabled_ = { true };

    TLayerLocationPtr PickLocation()
    {
        return DoPickLocation(Locations_, [] (const TLayerLocationPtr& candidate, const TLayerLocationPtr& current) {
            return candidate->GetVolumeCount() < current->GetVolumeCount();
        });
    }

    void OnLayersPrepared(
        TPromise<TVolumeStatePtr> volumeStatePromise,
        const TVolumeKey& key,
        TGuid tag,
        const TErrorOr<std::vector<TLayerPtr>>& errorOrLayers)
    {
        try {
            YT_LOG_DEBUG(errorOrLayers, "All layers prepared (Tag: %v)",
                tag);

            const auto& layers = errorOrLayers
                .ValueOrThrow();

            std::vector<TLayerMeta> layerMetas;
            layerMetas.reserve(layers.size());
            for (const auto& layer : layers) {
                layerMetas.push_back(layer->GetMeta());
            }

            auto location = PickLocation();
            auto volumeMeta = WaitFor(location->CreateVolume(layerMetas))
                .ValueOrThrow();

            auto volumeState = New<TVolumeState>(
                volumeMeta,
                this,
                location,
                layers);

            YT_LOG_DEBUG("Created volume state (Tag: %v, VolumeId: %v)",
                tag,
                volumeMeta.Id);

            volumeStatePromise.TrySet(volumeState);
        } catch (const std::exception& ex) {
            volumeStatePromise.TrySet(TError(ex));
        }
    }
};

DEFINE_REFCOUNTED_TYPE(TPortoVolumeManager)

////////////////////////////////////////////////////////////////////////////////

IVolumeManagerPtr CreatePortoVolumeManager(
    TVolumeManagerConfigPtr config,
    TBootstrap* bootstrap)
{
    return New<TPortoVolumeManager>(std::move(config), bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode

#endif
