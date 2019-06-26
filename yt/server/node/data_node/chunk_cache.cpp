#include "chunk_cache.h"
#include "private.h"
#include "artifact.h"
#include "blob_chunk.h"
#include "blob_reader_cache.h"
#include "chunk_block_manager.h"
#include "config.h"
#include "location.h"
#include "master_connector.h"

#include <yt/server/node/cell_node/bootstrap.h>

#include <yt/client/formats/config.h>

#include <yt/client/api/config.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/client/chunk_client/proto/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/ytlib/chunk_client/data_source.h>
#include <yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/ytlib/chunk_client/file_writer.h>
#include <yt/ytlib/chunk_client/file_reader.h>
#include <yt/ytlib/chunk_client/replication_reader.h>
#include <yt/ytlib/chunk_client/block_fetcher.h>

#include <yt/ytlib/file_client/file_chunk_reader.h>

#include <yt/client/formats/format.h>

#include <yt/ytlib/hydra/peer_channel.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/table_client/helpers.h>
#include <yt/client/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_chunk_reader.h>

#include <yt/core/concurrency/async_stream.h>
#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/fs.h>
#include <yt/core/misc/serialize.h>
#include <yt/core/misc/string.h>

#include <util/random/random.h>

namespace NYT::NDataNode {

using namespace NYTree;
using namespace NYson;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NFileClient;
using namespace NNodeTrackerClient;
using namespace NTableClient;
using namespace NCellNode;
using namespace NRpc;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NApi;
using namespace NFormats;

using NChunkClient::TDataSliceDescriptor;
using NChunkClient::TChunkReaderStatistics;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;
static const int TableArtifactBufferRowCount = 10000;

////////////////////////////////////////////////////////////////////////////////

class TSessionCounterGuard
{
public:
    explicit TSessionCounterGuard(TLocationPtr location)
        : Location_(std::move(location))
    {
        Location_->UpdateSessionCount(ESessionType::User, +1);
    }

    TSessionCounterGuard(TSessionCounterGuard&& other) = default;

    ~TSessionCounterGuard()
    {
        if (Location_) {
            Location_->UpdateSessionCount(ESessionType::User, -1);
        }
    }

private:
    TLocationPtr Location_;
};

////////////////////////////////////////////////////////////////////////////////

class TErrorInterceptingOutput
    : public IOutputStream
{
public:
    TErrorInterceptingOutput(TLocationPtr location, IOutputStream* underlying)
        : Location_(std::move(location))
        , Underlying_(underlying)
    { }

private:
    const TLocationPtr Location_;
    IOutputStream* const Underlying_;


    virtual void DoWrite(const void* buf, size_t len) override
    {
        try {
            Underlying_->Write(buf, len);
        } catch (const std::exception& ex) {
            Location_->Disable(ex);
            Y_UNREACHABLE();
        }
    }

    virtual void DoWriteV(const TPart* parts, size_t count) override
    {
        try {
            Underlying_->Write(parts, count);
        } catch (const std::exception& ex) {
            Location_->Disable(ex);
            Y_UNREACHABLE();
        }
    }

    virtual void DoFlush() override
    {
        try {
            Underlying_->Flush();
        } catch (const std::exception& ex) {
            Location_->Disable(ex);
            Y_UNREACHABLE();
        }
    }

    virtual void DoFinish() override
    {
        try {
            Underlying_->Finish();
        } catch (const std::exception& ex) {
            Location_->Disable(ex);
            Y_UNREACHABLE();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TErrorInterceptingChunkWriter
    : public IChunkWriter
{
public:
    TErrorInterceptingChunkWriter(TLocationPtr location, IChunkWriterPtr underlying)
        : Location_(std::move(location))
        , Underlying_(std::move(underlying))
    { }

    virtual TFuture<void> Open() override
    {
        return Check(Underlying_->Open());
    }

    virtual bool WriteBlock(const TBlock& block) override
    {
        return Underlying_->WriteBlock(block);
    }

    virtual bool WriteBlocks(const std::vector<TBlock>& blocks) override
    {
        return Underlying_->WriteBlocks(blocks);
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return Check(Underlying_->GetReadyEvent());
    }

    virtual TFuture<void> Close(const NChunkClient::TRefCountedChunkMetaPtr& chunkMeta) override
    {
        return Check(Underlying_->Close(chunkMeta));
    }

    virtual const NChunkClient::NProto::TChunkInfo& GetChunkInfo() const override
    {
        return Underlying_->GetChunkInfo();
    }

    virtual TChunkReplicaWithMediumList GetWrittenChunkReplicas() const override
    {
        return Underlying_->GetWrittenChunkReplicas();
    }

    virtual TChunkId GetChunkId() const override
    {
        return Underlying_->GetChunkId();
    }

    virtual NErasure::ECodec GetErasureCodecId() const override
    {
        return Underlying_->GetErasureCodecId();
    }

    virtual const NChunkClient::NProto::TDataStatistics& GetDataStatistics() const override
    {
        return Underlying_->GetDataStatistics();
    }

    virtual bool HasSickReplicas() const override
    {
        return Underlying_->HasSickReplicas();
    }

private:
    const TLocationPtr Location_;
    const IChunkWriterPtr Underlying_;


    TFuture<void> Check(TFuture<void> result)
    {
        return result.Apply(BIND([location = Location_] (const TError& error) {
            if (!error.IsOK()) {
                location->Disable(error);
                Y_UNREACHABLE();
            }
        }));
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TArtifactMetaHeader
{
    ui64 Signature = ExpectedSignature;
    ui64 Version = ExpectedVersion;

    static constexpr ui64 ExpectedSignature = 0x313030484d415459ull; // YTAMH001
    static constexpr ui64 ExpectedVersion = 4;
};

constexpr ui64 TArtifactMetaHeader::ExpectedSignature;
constexpr ui64 TArtifactMetaHeader::ExpectedVersion;

struct TArtifactReaderMetaBufferTag { };

////////////////////////////////////////////////////////////////////////////////

class TChunkCache::TImpl
    : public TAsyncSlruCacheBase<TArtifactKey, TCachedBlobChunk>
{
public:
    DEFINE_BYREF_RO_PROPERTY(std::vector<TCacheLocationPtr>, Locations);

public:
    TImpl(TDataNodeConfigPtr config, TBootstrap* bootstrap)
        : TAsyncSlruCacheBase(
            New<TSlruCacheConfig>(config->GetCacheCapacity()),
            DataNodeProfiler.AppendPath("/chunk_cache"))
        , Config_(config)
        , Bootstrap_(bootstrap)
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetControlInvoker(), ControlThread);
    }

    void Initialize()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_INFO("Initializing chunk cache");

        std::vector<TFuture<std::vector<TChunkDescriptor>>> asyncDescriptors;
        asyncDescriptors.reserve(Config_->CacheLocations.size());

        for (int i = 0; i < Config_->CacheLocations.size(); ++i) {
            auto locationConfig = Config_->CacheLocations[i];

            auto location = New<TCacheLocation>(
                "cache" + ToString(i),
                locationConfig,
                Bootstrap_);

            asyncDescriptors.push_back(
                BIND(&TCacheLocation::Scan, location)
                    .AsyncVia(location->GetWritePoolInvoker())
                    .Run());

            Locations_.push_back(location);
        }

        auto allDescriptors = WaitFor(Combine(asyncDescriptors))
            .ValueOrThrow();

        for (int index = 0; index < Config_->CacheLocations.size(); ++index) {
            const auto& location = Locations_[index];

            for (const auto& descriptor : allDescriptors[index]) {
                RegisterChunk(location, descriptor);
            }

            location->Start();
        }

        ValidateLocationMedia();

        YT_LOG_INFO("Chunk cache initialized, %v chunks total",
            GetSize());
    }

    void ValidateLocationMedia()
    {
        if (Locations_.empty()) {
            return;
        }

        auto mediumName = Locations_.front()->GetMediumName();

        for (const auto& location : Locations_) {
            if (location->GetMediumName() != mediumName) {
                THROW_ERROR_EXCEPTION(
                    "Locations %v and %v are configured with distinct media (%Qv != %Qv), "
                    "but multiple cache media on one host are not supported yet",
                    Locations_.front()->GetId(),
                    location->GetId(),
                    mediumName,
                    location->GetMediumName());

            }
        }
    }

    bool IsEnabled() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        for (const auto& location : Locations_) {
            if (location->IsEnabled()) {
                return true;
            }
        }
        return false;
    }

    IChunkPtr FindChunk(TChunkId chunkId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Find(TArtifactKey(chunkId));
    }

    std::vector<IChunkPtr> GetChunks()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto chunks = GetAll();
        return std::vector<IChunkPtr>(chunks.begin(), chunks.end());
    }

    TFuture<IChunkPtr> DownloadArtifact(
        const TArtifactKey& key,
        const TArtifactDownloadOptions& options)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto blockReadOptions = MakeClientBlockReadOptions();

        auto Logger = NLogging::TLogger(DataNodeLogger)
            .AddTag("Key: %v, ReadSessionId: %v", key, blockReadOptions.ReadSessionId);

        auto cookie = BeginInsert(key);
        auto cookieValue = cookie.GetValue();
        if (cookie.IsActive()) {
            YT_LOG_INFO("Loading artifact into cache");

            auto canPrepareSingleChunk = CanPrepareSingleChunk(key);
            auto chunkId = GetOrCreateArtifactId(key, canPrepareSingleChunk);

            auto location = FindNewChunkLocation();
            if (!location) {
                auto error = TError("Cannot find a suitable location for artifact chunk");
                cookie.Cancel(error);
                YT_LOG_ERROR(error);
                return cookieValue.As<IChunkPtr>();
            }

            decltype(&TImpl::DownloadChunk) downloader;
            if (canPrepareSingleChunk) {
                downloader = &TImpl::DownloadChunk;
            } else {
                switch (CheckedEnumCast<EDataSourceType>(key.data_source().type())) {
                    case EDataSourceType::File:
                        downloader = &TImpl::DownloadFile;
                        break;
                    case EDataSourceType::UnversionedTable:
                    case EDataSourceType::VersionedTable:
                        downloader = &TImpl::DownloadTable;
                        break;
                    default:
                        Y_UNREACHABLE();
                }
            }

            TSessionCounterGuard guard(location);

            auto invoker = CreateSerializedInvoker(location->GetWritePoolInvoker());
            invoker->Invoke(BIND(
                downloader,
                MakeStrong(this),
                Passed(std::move(guard)),
                key,
                location,
                chunkId,
                options.NodeDirectory ? options.NodeDirectory : New<TNodeDirectory>(),
                blockReadOptions,
                Passed(std::move(cookie)),
                options.TrafficMeter));

        } else {
            YT_LOG_INFO("Artifact is already cached");
        }
        return cookieValue.As<IChunkPtr>();
    }

    std::function<void(IOutputStream*)> MakeArtifactDownloadProducer(
        const TArtifactKey& key,
        const TArtifactDownloadOptions& options)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto blockReadOptions = MakeClientBlockReadOptions();

        decltype(&TImpl::MakeFileProducer) producerBuilder;
        switch (CheckedEnumCast<EDataSourceType>(key.data_source().type())) {
            case EDataSourceType::File:
                producerBuilder = &TImpl::MakeFileProducer;
                break;
            case EDataSourceType::UnversionedTable:
            case EDataSourceType::VersionedTable:
                producerBuilder = &TImpl::MakeTableProducer;
                break;
            default:
                Y_UNREACHABLE();
        }

        return (this->*producerBuilder)(
            key,
            options.NodeDirectory ? options.NodeDirectory : New<TNodeDirectory>(),
            options.TrafficMeter,
            blockReadOptions,
            // TODO(babenko): throttle prepartion
            GetUnlimitedThrottler());
    }

private:
    const TDataNodeConfigPtr Config_;
    TBootstrap* const Bootstrap_;

    DEFINE_SIGNAL(void(IChunkPtr), ChunkAdded);
    DEFINE_SIGNAL(void(IChunkPtr), ChunkRemoved);

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);


    void OnChunkCreated(
        const TCacheLocationPtr& location,
        const TChunkDescriptor& descriptor)
    {
        Bootstrap_->GetControlInvoker()->Invoke(BIND([=] () {
            location->UpdateChunkCount(+1);
            location->UpdateUsedSpace(+descriptor.DiskSpace);
        }));
    }

    void OnChunkDestroyed(
        const TCacheLocationPtr& location,
        const TChunkDescriptor& descriptor)
    {
        location->GetWritePoolInvoker()->Invoke(BIND(
            &TCacheLocation::RemoveChunkFilesPermanently,
            location,
            descriptor.Id));

        Bootstrap_->GetControlInvoker()->Invoke(BIND([=] () {
            location->UpdateChunkCount(-1);
            location->UpdateUsedSpace(-descriptor.DiskSpace);
        }));
    }

    TCachedBlobChunkPtr CreateChunk(
        TCacheLocationPtr location,
        const TArtifactKey& key,
        const TChunkDescriptor& descriptor,
        const NChunkClient::TRefCountedChunkMetaPtr meta = nullptr)
    {
        auto chunk = New<TCachedBlobChunk>(
            Bootstrap_,
            location,
            descriptor,
            meta,
            key,
            BIND(&TImpl::OnChunkDestroyed, MakeStrong(this), location, descriptor));

        OnChunkCreated(location, descriptor);
        return chunk;
    }

    void RegisterChunk(
        const TCacheLocationPtr& location,
        const TChunkDescriptor& descriptor)
    {
        const auto& chunkId = descriptor.Id;

        auto optionalKey = TryParseArtifactMeta(location, chunkId);
        if (!optionalKey) {
            return;
        }

        const auto& key = *optionalKey;
        auto cookie = BeginInsert(key);
        if (!cookie.IsActive()) {
            YT_LOG_WARNING("Removing duplicate cached chunk (ChunkId: %v)",
                chunkId);
            location->RemoveChunkFilesPermanently(chunkId);
            return;
        }

        auto chunk = CreateChunk(location, key, descriptor);
        cookie.EndInsert(chunk);
        YT_LOG_DEBUG("Cached chunk registered (ChunkId: %v, DiskSpace: %v)",
            chunkId,
            descriptor.DiskSpace);
    }

    virtual i64 GetWeight(const TCachedBlobChunkPtr& chunk) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return chunk->GetInfo().disk_space();
    }

    virtual void OnAdded(const TCachedBlobChunkPtr& chunk) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncSlruCacheBase::OnAdded(chunk);

        ChunkAdded_.Fire(chunk);
    }

    virtual void OnRemoved(const TCachedBlobChunkPtr& chunk) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAsyncSlruCacheBase::OnRemoved(chunk);

        ChunkRemoved_.Fire(chunk);
    }

    TCacheLocationPtr FindNewChunkLocation() const
    {
        std::vector<TCacheLocationPtr> candidates;
        for (const auto& location : Locations_) {
            if (location->IsEnabled()) {
                candidates.push_back(location);
            }
        }

        if (candidates.empty()) {
            return nullptr;
        }

        return *std::min_element(
            candidates.begin(),
            candidates.end(),
            [] (const TCacheLocationPtr& lhs, const TCacheLocationPtr& rhs) -> bool {
                if (lhs->GetSessionCount() < rhs->GetSessionCount()) {
                    return true;
                }
                return lhs->GetAvailableSpace() > rhs->GetAvailableSpace();
            });
    }

    TChunkId GetOrCreateArtifactId(const TArtifactKey& key, bool canPrepareSingleChunk)
    {
        if (canPrepareSingleChunk) {
            YCHECK(key.chunk_specs_size() == 1);
            const auto& chunkSpec = key.chunk_specs(0);
            return FromProto<TChunkId>(chunkSpec.chunk_id());
        } else {
            return TChunkId(
                static_cast<ui32>(TInstant::Now().MicroSeconds()),
                static_cast<ui32>(EObjectType::Artifact),
                RandomNumber<ui32>(),
                RandomNumber<ui32>());
        }
    }

    bool CanPrepareSingleChunk(const TArtifactKey& key)
    {
        if (EDataSourceType(key.data_source().type()) != EDataSourceType::File) {
            return false;
        }
        if (key.chunk_specs_size() != 1) {
            return false;
        }

        const auto& chunk = key.chunk_specs(0);
        if (chunk.has_lower_limit() && !IsTrivial(chunk.lower_limit())) {
            return false;
        }
        if (chunk.has_upper_limit() && !IsTrivial(chunk.upper_limit())) {
            return false;
        }

        auto miscExt = GetProtoExtension<TMiscExt>(chunk.chunk_meta().extensions());
        NCompression::ECodec compressionCodecId;
        if (!TryEnumCast(miscExt.compression_codec(), &compressionCodecId) ||
            compressionCodecId != NCompression::ECodec::None)
        {
            return false;
        }

        auto chunkId = FromProto<TChunkId>(chunk.chunk_id());
        if (IsErasureChunkId(chunkId)) {
            return false;
        }

        return true;
    }

    TClientBlockReadOptions MakeClientBlockReadOptions()
    {
        TClientBlockReadOptions options;
        options.WorkloadDescriptor = Config_->ArtifactCacheReader->WorkloadDescriptor;
        options.ChunkReaderStatistics = New<TChunkReaderStatistics>();
        options.ReadSessionId = TReadSessionId::Create();
        return options;
    }

    void DownloadChunk(
        TSessionCounterGuard /* sessionCounterGuard */,
        const TArtifactKey& key,
        const TCacheLocationPtr& location,
        TChunkId chunkId,
        const TNodeDirectoryPtr& nodeDirectory,
        const TClientBlockReadOptions& blockReadOptions,
        TInsertCookie cookie,
        const TTrafficMeterPtr& trafficMeter)
    {
        const auto& chunkSpec = key.chunk_specs(0);
        auto seedReplicas = FromProto<TChunkReplicaList>(chunkSpec.replicas());

        auto Logger = NLogging::TLogger(DataNodeLogger)
            .AddTag("ChunkId: %v, ReadSessionId: %v, Location: %v",
                chunkId,
                blockReadOptions.ReadSessionId,
                location->GetId());

        try {
            auto options = New<TRemoteReaderOptions>();
            options->EnableP2P = true;

            auto chunkReader = CreateReplicationReader(
                Config_->ArtifactCacheReader,
                options,
                Bootstrap_->GetMasterClient(),
                nodeDirectory,
                Bootstrap_->GetMasterConnector()->GetLocalDescriptor(),
                Bootstrap_->GetMasterConnector()->GetNodeId(),
                chunkId,
                seedReplicas,
                Bootstrap_->GetBlockCache(),
                trafficMeter,
                Bootstrap_->GetArtifactCacheInThrottler(),
                Bootstrap_->GetReadRpsOutThrottler());

            auto fileName = location->GetChunkPath(chunkId);
            auto chunkWriter = New<TFileWriter>(
                location->GetIOEngine(),
                chunkId,
                fileName,
                /* syncOnClose */ true);

            auto checkedChunkWriter = New<TErrorInterceptingChunkWriter>(location, chunkWriter);

            YT_LOG_DEBUG("Opening chunk writer");

            WaitFor(checkedChunkWriter->Open())
                .ThrowOnError();

            YT_LOG_DEBUG("Getting chunk meta");

            auto chunkMeta = WaitFor(chunkReader->GetMeta(
                blockReadOptions))
                .ValueOrThrow();

            // Download all blocks.
            auto blocksExt = GetProtoExtension<TBlocksExt>(chunkMeta->extensions());
            int blockCount = blocksExt.blocks_size();
            std::vector<TBlockFetcher::TBlockInfo> blocks;
            blocks.reserve(blockCount);
            for (int index = 0; index < blockCount; ++index) {
                blocks.push_back(TBlockFetcher::TBlockInfo(
                    index,
                    blocksExt.blocks(index).size(),
                    index /* priority */));
            }

            auto asyncSemaphore = New<TAsyncSemaphore>(Config_->ArtifactCacheReader->WindowSize);

            auto blockFetcher = New<TBlockFetcher>(
                Config_->ArtifactCacheReader,
                std::move(blocks),
                asyncSemaphore,
                chunkReader,
                GetNullBlockCache(),
                NCompression::ECodec::None,
                1.0, /* compressionRatio */
                blockReadOptions);

            for (int index = 0; index < blockCount; ++index) {
                YT_LOG_DEBUG("Downloading block (BlockIndex: %v)",
                    index);

                auto block = WaitFor(blockFetcher->FetchBlock(index))
                    .ValueOrThrow();

                YT_LOG_DEBUG("Writing block (BlockIndex: %v)",
                    index);

                if (!checkedChunkWriter->WriteBlock(block)) {
                    WaitFor(chunkWriter->GetReadyEvent())
                        .ThrowOnError();
                }

                WaitFor(location->GetInThrottler()->Throttle(block.Size()))
                    .ThrowOnError();
            }

            YT_LOG_DEBUG("Closing chunk");

            WaitFor(checkedChunkWriter->Close(chunkMeta))
                .ThrowOnError();

            YT_LOG_INFO("Chunk is downloaded into cache");

            TChunkDescriptor descriptor(chunkId);
            descriptor.DiskSpace = chunkWriter->GetChunkInfo().disk_space();
            auto chunk = CreateChunk(location, key, descriptor, chunkMeta);
            cookie.EndInsert(chunk);

            ChunkAdded_.Fire(chunk);
        } catch (const std::exception& ex) {
            auto error = TError("Error downloading chunk %v into cache",
                chunkId)
                << ex;
            cookie.Cancel(error);
            YT_LOG_WARNING(error);
        }
    }

    void DownloadFile(
        TSessionCounterGuard /* sessionCounterGuard */,
        const TArtifactKey& key,
        const TCacheLocationPtr& location,
        TChunkId chunkId,
        const TNodeDirectoryPtr& nodeDirectory,
        const TClientBlockReadOptions& blockReadOptions,
        TInsertCookie cookie,
        const TTrafficMeterPtr& trafficMeter)
    {
        try {
            auto producer = MakeFileProducer(
                key,
                nodeDirectory,
                trafficMeter,
                blockReadOptions,
                location->GetInThrottler());

            auto chunk = ProduceArtifactFile(key, location, chunkId, producer);
            cookie.EndInsert(chunk);

            ChunkAdded_.Fire(chunk);
        } catch (const std::exception& ex) {
            auto error = TError("Error downloading file artifact into cache")
                << TErrorAttribute("key", key)
                << ex;
            cookie.Cancel(error);
            YT_LOG_WARNING(error);
        }
    }

    std::function<void(IOutputStream*)> MakeFileProducer(
        const TArtifactKey& key,
        const TNodeDirectoryPtr& nodeDirectory,
        const TTrafficMeterPtr& trafficMeter,
        const TClientBlockReadOptions& blockReadOptions,
        const IThroughputThrottlerPtr& throttler)
    {
        std::vector<TChunkSpec> chunkSpecs(key.chunk_specs().begin(), key.chunk_specs().end());

        auto readerOptions = New<TMultiChunkReaderOptions>();
        readerOptions->EnableP2P = true;

        auto reader = CreateFileMultiChunkReader(
            Config_->ArtifactCacheReader,
            readerOptions,
            Bootstrap_->GetMasterClient(),
            Bootstrap_->GetMasterConnector()->GetLocalDescriptor(),
            Bootstrap_->GetMasterConnector()->GetNodeId(),
            Bootstrap_->GetBlockCache(),
            nodeDirectory,
            blockReadOptions,
            chunkSpecs,
            trafficMeter,
            Bootstrap_->GetArtifactCacheInThrottler(),
            Bootstrap_->GetReadRpsOutThrottler());

        return [=] (IOutputStream* output) {
            TBlock block;
            while (reader->ReadBlock(&block)) {
                if (block.Data.Empty()) {
                    WaitFor(reader->GetReadyEvent())
                        .ThrowOnError();
                } else {
                    output->Write(block.Data.Begin(), block.Size());
                    WaitFor(throttler->Throttle(block.Size()))
                        .ThrowOnError();
                }
            }
        };
    }

    void DownloadTable(
        TSessionCounterGuard /* sessionCounterGuard */,
        const TArtifactKey& key,
        const TCacheLocationPtr& location,
        TChunkId chunkId,
        const TNodeDirectoryPtr& nodeDirectory,
        const TClientBlockReadOptions& blockReadOptions,
        TInsertCookie cookie,
        const TTrafficMeterPtr& trafficMeter)
    {
        try {
            auto producer = MakeTableProducer(
                key,
                nodeDirectory,
                trafficMeter,
                blockReadOptions,
                location->GetInThrottler());

            auto chunk = ProduceArtifactFile(key, location, chunkId, producer);
            cookie.EndInsert(chunk);

            ChunkAdded_.Fire(chunk);
        } catch (const std::exception& ex) {
            auto error = TError("Error downloading table artifact into cache")
                << TErrorAttribute("key", key)
                << ex;
            cookie.Cancel(error);
        }
    }

    std::function<void(IOutputStream*)> MakeTableProducer(
        const TArtifactKey& key,
        const TNodeDirectoryPtr& nodeDirectory,
        const TTrafficMeterPtr& trafficMeter,
        const TClientBlockReadOptions& blockReadOptions,
        const IThroughputThrottlerPtr& throttler)
    {
        static const TString CachedSourcePath = "<cached_data_source>";

        auto nameTable = New<TNameTable>();

        auto readerOptions = New<NTableClient::TTableReaderOptions>();
        readerOptions->EnableP2P = true;

        std::vector<TDataSliceDescriptor> dataSliceDescriptors;
        auto dataSourceDirectory = New<NChunkClient::TDataSourceDirectory>();

        std::optional<TTableSchema> schema = key.data_source().has_table_schema()
            ? std::make_optional(FromProto<TTableSchema>(key.data_source().table_schema()))
            : std::nullopt;

        auto columnFilter = key.data_source().has_column_filter()
            ? std::make_optional(FromProto<std::vector<TString>>(key.data_source().columns()))
            : std::nullopt;

        switch (EDataSourceType(key.data_source().type())) {
            case EDataSourceType::UnversionedTable:
                dataSourceDirectory->DataSources().push_back(MakeUnversionedDataSource(
                    CachedSourcePath,
                    schema,
                    columnFilter,
                    /* omittedInaccessibleColumns */ {}));
                for (const auto& chunkSpec : key.chunk_specs()) {
                    dataSliceDescriptors.push_back(TDataSliceDescriptor(chunkSpec));
                }
                break;

            case EDataSourceType::VersionedTable:
                YCHECK(schema);
                dataSourceDirectory->DataSources().push_back(MakeVersionedDataSource(
                    CachedSourcePath,
                    *schema,
                    columnFilter,
                    /* omittedInaccessibleColumns */ {},
                    key.data_source().timestamp()));
                dataSliceDescriptors.push_back(TDataSliceDescriptor(FromProto<std::vector<TChunkSpec>>(key.chunk_specs())));
                break;

            default:
                Y_UNREACHABLE();
        }

        auto reader = CreateSchemalessSequentialMultiReader(
            Config_->ArtifactCacheReader,
            readerOptions,
            Bootstrap_->GetMasterClient(),
            Bootstrap_->GetMasterConnector()->GetLocalDescriptor(),
            Bootstrap_->GetMasterConnector()->GetNodeId(),
            Bootstrap_->GetBlockCache(),
            nodeDirectory,
            dataSourceDirectory,
            std::move(dataSliceDescriptors),
            nameTable,
            blockReadOptions,
            /* columnFilter */ {},
            /* keyColumns */ {},
            /* partitionTag */ std::nullopt,
            trafficMeter,
            Bootstrap_->GetArtifactCacheInThrottler(),
            Bootstrap_->GetReadRpsOutThrottler());

        auto format = ConvertTo<NFormats::TFormat>(TYsonString(key.format()));

        return [=] (IOutputStream* output) {
            auto writer = CreateStaticTableWriterForFormat(
                format,
                nameTable,
                {schema.value_or(TTableSchema())},
                CreateAsyncAdapter(output),
                false, /* enableContextSaving */
                New<TControlAttributesConfig>(),
                0);
            TPipeReaderToWriterOptions options;
            options.BufferRowCount = TableArtifactBufferRowCount;
            options.Throttler = throttler;
            PipeReaderToWriter(
                reader,
                writer,
                options);
        };
    }

    TCachedBlobChunkPtr ProduceArtifactFile(
        const TArtifactKey& key,
        const TCacheLocationPtr& location,
        TChunkId chunkId,
        const std::function<void(IOutputStream*)>& producer)
    {
        YT_LOG_INFO("Producing artifact file (ChunkId: %v, Location: %v)",
            chunkId,
            location->GetId());

        auto dataFileName = location->GetChunkPath(chunkId);
        auto metaFileName = dataFileName + ArtifactMetaSuffix;
        auto tempDataFileName = dataFileName + NFS::TempFileSuffix;
        auto tempMetaFileName = metaFileName + NFS::TempFileSuffix;

        auto metaBlob = SerializeProtoToRef(key);
        TArtifactMetaHeader metaHeader;

        std::unique_ptr<TFile> tempDataFile;
        std::unique_ptr<TFile> tempMetaFile;
        i64 chunkSize;

        location->DisableOnError(BIND([&] () {
            tempDataFile = std::make_unique<TFile>(
                tempDataFileName,
                CreateAlways | WrOnly | Seq | CloseOnExec);
            tempDataFile->Flock(LOCK_EX);

            tempMetaFile = std::make_unique<TFile>(
                tempMetaFileName,
                CreateAlways | WrOnly | Seq | CloseOnExec);
            tempMetaFile->Flock(LOCK_EX);
        })).Run();

        TUnbufferedFileOutput fileOutput(*tempDataFile);
        TErrorInterceptingOutput checkedOutput(location, &fileOutput);

        producer(&checkedOutput);

        location->DisableOnError(BIND([&] () {
            chunkSize = tempDataFile->GetLength();
            tempDataFile->Flush();
            tempDataFile->Close();

            tempMetaFile->Write(static_cast<void*>(&metaHeader), sizeof(TArtifactMetaHeader));
            tempMetaFile->Write(metaBlob.Begin(), metaBlob.Size());
            tempMetaFile->Flush();
            tempMetaFile->Close();

            NFS::Rename(tempMetaFileName, metaFileName);
            NFS::Rename(tempDataFileName, dataFileName);
        })).Run();

        TChunkDescriptor descriptor(chunkId);
        descriptor.DiskSpace = chunkSize + metaBlob.Size();
        return CreateChunk(location, key, descriptor);
    }

    std::optional<TArtifactKey> TryParseArtifactMeta(
        const TCacheLocationPtr& location,
        TChunkId chunkId)
    {
        if (!IsArtifactChunkId(chunkId)) {
            // NB(psushin): cached chunks (non-artifacts) are not fsynced when written. This may result in truncated or even empty
            // files on power loss. To detect corrupted chunks we validate their size against value in misc extension.

            auto dataFileName = location->GetChunkPath(chunkId);

            auto chunkReader = New<TFileReader>(
                location->GetIOEngine(),
                chunkId,
                dataFileName);

            TClientBlockReadOptions blockReadOptions{
                Config_->ArtifactCacheReader->WorkloadDescriptor,
                New<TChunkReaderStatistics>(),
                TReadSessionId::Create() };

            auto metaOrError = WaitFor(chunkReader->GetMeta(blockReadOptions));

            if (!metaOrError.IsOK()) {
                YT_LOG_WARNING(metaOrError, "Failed to read cached chunk meta (ChunkId: %v)",
                    chunkId);
                location->RemoveChunkFilesPermanently(chunkId);
                return std::nullopt;
            }

            const auto& meta = *metaOrError.Value();
            auto miscExt = GetProtoExtension<TMiscExt>(meta.extensions());

            try {
                TFile dataFile(dataFileName, OpenExisting);
                if (dataFile.GetLength() != miscExt.compressed_data_size()) {
                    YT_LOG_WARNING("Failed to validate cached chunk size (ChunkId: %v, ExpectedSize: %v, ActualSize: %v)",
                        chunkId,
                        miscExt.compressed_data_size(),
                        dataFile.GetLength());
                    location->RemoveChunkFilesPermanently(chunkId);
                    return std::nullopt;
                }
            } catch (const std::exception& ex) {
                YT_LOG_WARNING(ex, "Failed to validate cached chunk size (ChunkId: %v)",
                    chunkId);
                location->RemoveChunkFilesPermanently(chunkId);
                return std::nullopt;
            }

            return TArtifactKey(chunkId);
        }

        auto dataFileName = location->GetChunkPath(chunkId);
        auto metaFileName = dataFileName + ArtifactMetaSuffix;

        TSharedMutableRef metaBlob;

        location->DisableOnError(BIND([&] () {
            TFile metaFile(
                metaFileName,
                OpenExisting | RdOnly | Seq | CloseOnExec);
            TFileInput metaInput(metaFile);
            metaBlob = TSharedMutableRef::Allocate<TArtifactReaderMetaBufferTag>(metaFile.GetLength());
            metaInput.Read(metaBlob.Begin(), metaFile.GetLength());
        })).Run();

        auto readMeta = [&] () -> std::optional<TArtifactKey> {
            if (metaBlob.Size() < sizeof(TArtifactMetaHeader)) {
                YT_LOG_WARNING("Artifact meta file %v is too short: at least %v bytes expected",
                    metaFileName,
                    sizeof(TArtifactMetaHeader));
                return std::nullopt;
            }

            const auto* header = reinterpret_cast<const TArtifactMetaHeader*>(metaBlob.Begin());
            if (header->Signature != header->ExpectedSignature) {
                YT_LOG_WARNING("Bad signature in artifact meta file %v: expected %X, actual %X",
                    metaFileName,
                    header->ExpectedSignature,
                    header->Signature);
                return std::nullopt;
            }

            if (header->Version != header->ExpectedVersion) {
                YT_LOG_WARNING("Incompatible version in artifact meta file %v: expected %v, actual %v",
                    metaFileName,
                    header->ExpectedVersion,
                    header->Version);
                return std::nullopt;
            }

            metaBlob = metaBlob.Slice(sizeof(TArtifactMetaHeader), metaBlob.Size());
            TArtifactKey key;
            if (!TryDeserializeProto(&key, metaBlob)) {
                YT_LOG_WARNING("Failed to parse artifact meta file %v",
                    metaFileName);
                return std::nullopt;
            }

            return key;
        };

        auto key = readMeta();
        if (!key) {
            location->RemoveChunkFilesPermanently(chunkId);
        }
        return key;
    }

};

////////////////////////////////////////////////////////////////////////////////

TChunkCache::TChunkCache(TDataNodeConfigPtr config, TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TChunkCache::~TChunkCache() = default;

void TChunkCache::Initialize()
{
    Impl_->Initialize();
}

bool TChunkCache::IsEnabled() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->IsEnabled();
}

IChunkPtr TChunkCache::FindChunk(TChunkId chunkId)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->FindChunk(chunkId);
}

std::vector<IChunkPtr> TChunkCache::GetChunks()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->GetChunks();
}

int TChunkCache::GetChunkCount()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->GetSize();
}

TFuture<IChunkPtr> TChunkCache::DownloadArtifact(
    const TArtifactKey& key,
    const TArtifactDownloadOptions& options)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->DownloadArtifact(key, options);
}

std::function<void(IOutputStream*)> TChunkCache::MakeArtifactDownloadProducer(
    const TArtifactKey& key,
    const TArtifactDownloadOptions& options)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Impl_->MakeArtifactDownloadProducer(key, options);
}

DELEGATE_BYREF_RO_PROPERTY(TChunkCache, std::vector<TCacheLocationPtr>, Locations, *Impl_);
DELEGATE_SIGNAL(TChunkCache, void(IChunkPtr), ChunkAdded, *Impl_);
DELEGATE_SIGNAL(TChunkCache, void(IChunkPtr), ChunkRemoved, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
