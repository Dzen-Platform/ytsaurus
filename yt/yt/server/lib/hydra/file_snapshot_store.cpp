#include "private.h"
#include "file_snapshot_store.h"

#include <yt/yt/server/lib/hydra_common/config.h>
#include <yt/yt/server/lib/hydra_common/file_helpers.h>
#include <yt/yt/server/lib/hydra_common/format.h>
#include <yt/yt/server/lib/hydra_common/snapshot.h>
#include <yt/yt/server/lib/hydra_common/private.h>

#include <yt/yt/core/actions/signal.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/checkpointable_stream.h>
#include <yt/yt/core/misc/checksum.h>
#include <yt/yt/core/misc/fs.h>
#include <yt/yt/core/misc/serialize.h>

#include <library/cpp/streams/lz/lz.h>

#include <util/stream/file.h>

namespace NYT::NHydra {

using namespace NFS;
using namespace NCompression;
using namespace NHydra::NProto;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TFileSnapshotReader)
DECLARE_REFCOUNTED_CLASS(TFileSnapshotWriter)

////////////////////////////////////////////////////////////////////////////////

static const i64 ReaderBlockSize = 1_MB;

////////////////////////////////////////////////////////////////////////////////

class TFileSnapshotReader
    : public ISnapshotReader
{
public:
    TFileSnapshotReader(
        const TString& fileName,
        int snapshotId,
        bool raw,
        std::optional<i64> offset,
        bool skipHeader)
        : FileName_(fileName)
        , SnapshotId_(snapshotId)
        , Raw_(raw)
        , Offset_(offset)
        , SkipHeader_(skipHeader)
        , Logger(HydraLogger.WithTag("Path: %v", FileName_))
    { }

    int GetSnapshotId() const
    {
        return SnapshotId_;
    }

    TFuture<void> Open() override
    {
        return BIND(&TFileSnapshotReader::DoOpen, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run();
    }

    TFuture<TSharedRef> Read() override
    {
        return BIND(&TFileSnapshotReader::DoRead, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run();
    }

    TSnapshotParams GetParams() const override
    {
        TSnapshotParams params;
        params.Meta = Meta_;
        params.Checksum = Header_.Checksum;
        params.CompressedLength = Header_.CompressedLength;
        params.UncompressedLength = Header_.UncompressedLength;
        return params;
    }

private:
    const TString FileName_;
    const int SnapshotId_;
    const bool Raw_;
    const std::optional<i64> Offset_;
    bool SkipHeader_;

    const NLogging::TLogger Logger;

    std::unique_ptr<TFile> File_;
    std::unique_ptr<TUnbufferedFileInput> FileInput_;
    std::unique_ptr<IInputStream> CodecInput_;
    IInputStream* FacadeInput_;

    TSnapshotHeader Header_;
    TSnapshotMeta Meta_;


    void DoOpen()
    {
        YT_LOG_DEBUG("Opening local snapshot reader (Raw: %v, Offset: %v)",
            Raw_,
            Offset_);

        try {
            File_.reset(new TFile(FileName_, OpenExisting | CloseOnExec));
            TUnbufferedFileInput input(*File_);

            if (SkipHeader_) {
                FileInput_.reset(new TUnbufferedFileInput(*File_));
                FacadeInput_ = FileInput_.get();
                YT_LOG_DEBUG("Local snapshot reader opened, assumed headerless snapshot");
                return;
            }

            ui64 signature;
            ReadPod(input, signature);

            if (signature != TSnapshotHeader::ExpectedSignature) {
                THROW_ERROR_EXCEPTION("Unrecognized snapshot signature %" PRIx64,
                    signature);
            }

            File_->Seek(0, sSet);
            ReadPod(input, Header_);

            if (Header_.SnapshotId != SnapshotId_ && SnapshotId_ != InvalidSegmentId) {
                THROW_ERROR_EXCEPTION(
                    "Invalid snapshot id in header of %v: expected %v, got %v",
                    FileName_,
                    SnapshotId_,
                    Header_.SnapshotId);
            }

            if (static_cast<i64>(Header_.CompressedLength) != File_->GetLength()) {
                THROW_ERROR_EXCEPTION(
                    "Invalid compressed length in header of %v: expected %v, got %v",
                    FileName_,
                    File_->GetLength(),
                    Header_.CompressedLength);
            }

            auto serializedMeta = TSharedMutableRef::Allocate(Header_.MetaSize, false);
            ReadRefPadded(input, serializedMeta);
            DeserializeProto(&Meta_, serializedMeta);

            if (Raw_) {
                YT_VERIFY(Offset_);
                File_->Seek(*Offset_, sSet);
            }

            FileInput_.reset(new TUnbufferedFileInput(*File_));

            if (Raw_) {
                FacadeInput_ = FileInput_.get();
            } else {
                auto codec = CheckedEnumCast<ECodec>(Header_.Codec);
                switch (codec) {
                    case ECodec::None:
                        break;
                    case ECodec::Snappy:
                        CodecInput_.reset(new TSnappyDecompress(FileInput_.get()));
                        break;
                    case ECodec::Lz4:
                        CodecInput_.reset(new TLz4Decompress(FileInput_.get()));
                        break;
                    default:
                        YT_ABORT();
                }
                FacadeInput_ = CodecInput_ ? CodecInput_.get() : FileInput_.get();
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error opening snapshot %v for reading",
                FileName_)
                << ex;
        }

        YT_LOG_DEBUG("Local snapshot reader opened");
    }

    TSharedRef DoRead()
    {
        auto block = TSharedMutableRef::Allocate(ReaderBlockSize, false);
        size_t length = FacadeInput_->Load(block.Begin(), block.Size());
        return length == 0 ? TSharedRef() : block.Slice(0, length);
    }
};

DEFINE_REFCOUNTED_TYPE(TFileSnapshotReader)

ISnapshotReaderPtr CreateFileSnapshotReader(
    const TString& fileName,
    int snapshotId,
    bool raw,
    std::optional<i64> offset,
    bool skipHeader)
{
    return New<TFileSnapshotReader>(
        fileName,
        snapshotId,
        raw,
        offset,
        skipHeader);
}

////////////////////////////////////////////////////////////////////////////////

class TFileSnapshotWriter
    : public ISnapshotWriter
{
public:
    TFileSnapshotWriter(
        const TString& fileName,
        ECodec codec,
        int snapshotId,
        const TSnapshotMeta& meta,
        bool raw)
        : FileName_(fileName)
        , Codec_(codec)
        , SnapshotId_(snapshotId)
        , Meta_(meta)
        , IsRaw_(raw)
    {
        SerializedMeta_ = SerializeProtoToRef(Meta_);
        Logger.AddTag("Path: %v", FileName_);
    }

    ~TFileSnapshotWriter()
    {
        // TODO(babenko): consider moving this code into HydraIO queue
        if (!IsClosed_) {
            try {
                DoFinish();
                File_->Close();
                NFS::Remove(FileName_ + TempFileSuffix);
            } catch (const std::exception& ex) {
                YT_LOG_WARNING(ex, "Error removing temporary local snapshot %v, ignored",
                    FileName_ + TempFileSuffix);
            }
        }
    }

    TFuture<void> Open() override
    {
        return BIND(&TFileSnapshotWriter::DoOpen, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run();
    }

    TFuture<void> Write(const TSharedRef& buffer) override
    {
        return BIND(&TFileSnapshotWriter::DoWrite, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run(buffer);
    }

    TFuture<void> Close() override
    {
        return BIND(&TFileSnapshotWriter::DoClose, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run();
    }

    TSnapshotParams GetParams() const override
    {
        YT_VERIFY(IsClosed_);
        return Params_;
    }

    DEFINE_SIGNAL(void(), Closed);

private:
    const TString FileName_;
    const ECodec Codec_;
    const int SnapshotId_;
    const TSnapshotMeta Meta_;
    const bool IsRaw_;

    TSharedRef SerializedMeta_;

    bool IsOpened_ = false;
    bool IsClosed_ = false;
    TSnapshotParams Params_;

    std::unique_ptr<TFile> File_;
    std::unique_ptr<TUnbufferedFileOutput> FileOutput_;
    std::unique_ptr<IOutputStream> CodecOutput_;
    std::unique_ptr<TChecksumOutput> ChecksumOutput_;
    std::unique_ptr<TLengthMeasureOutputStream> LengthMeasureOutput_;
    std::unique_ptr<IOutputStream> CheckpointableOutput_;
    IOutputStream* FacadeOutput_ = nullptr;

    NLogging::TLogger Logger = HydraLogger;


    void DoWrite(const TSharedRef& buffer)
    {
        YT_VERIFY(IsOpened_ && !IsClosed_);
        FacadeOutput_->Write(buffer.Begin(), buffer.Size());
    }

    void DoOpen()
    {
        YT_VERIFY(!IsOpened_);

        YT_LOG_DEBUG("Opening local snapshot writer (Codec: %v, Raw: %v)",
            Codec_,
            IsRaw_);

        try {
            File_.reset(new TFile(FileName_ + TempFileSuffix, CreateAlways | CloseOnExec));
            FileOutput_.reset(new TUnbufferedFileOutput(*File_));

            if (IsRaw_) {
                FacadeOutput_ = FileOutput_.get();
            } else {
                TSnapshotHeader header;
                Zero(header);
                WritePod(*File_, header);
                WriteRefPadded(*File_, SerializedMeta_);
                File_->Flush();

                ChecksumOutput_.reset(new TChecksumOutput(FileOutput_.get()));
                switch (Codec_) {
                    case ECodec::None:
                        break;
                    case ECodec::Snappy:
                        CodecOutput_.reset(new TSnappyCompress(ChecksumOutput_.get()));
                        break;
                    case ECodec::Lz4:
                        CodecOutput_.reset(new TLz4Compress(ChecksumOutput_.get()));
                        break;
                    default:
                        YT_ABORT();
                }
                LengthMeasureOutput_.reset(new TLengthMeasureOutputStream(CodecOutput_
                    ? CodecOutput_.get()
                    : ChecksumOutput_.get()));
                FacadeOutput_ = LengthMeasureOutput_.get();
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error opening snapshot %v for writing",
                FileName_)
                << ex;
        }

        IsOpened_ = true;

        YT_LOG_DEBUG("Local snapshot writer opened");
    }

    void DoFinish()
    {
        // NB: Some calls might be redundant.
        if (FacadeOutput_) {
            FacadeOutput_->Finish();
        }
        if (LengthMeasureOutput_) {
            LengthMeasureOutput_->Finish();
        }
        if (CodecOutput_) {
            CodecOutput_->Finish();
        }
        if (ChecksumOutput_) {
            ChecksumOutput_->Finish();
        }
        if (FileOutput_) {
            FileOutput_->Finish();
        }
    }

    void DoClose()
    {
        YT_VERIFY(IsOpened_ && !IsClosed_);

        YT_LOG_DEBUG("Closing local snapshot writer");

        DoFinish();

        Params_.Meta = Meta_;
        if (ChecksumOutput_) {
            Params_.Checksum = ChecksumOutput_->GetChecksum();
        }
        Params_.CompressedLength = File_->GetLength();
        if (LengthMeasureOutput_) {
            Params_.UncompressedLength = LengthMeasureOutput_->GetLength();
        }

        if (!IsRaw_) {
            TSnapshotHeader header;
            Zero(header);
            header.Signature = TSnapshotHeader::ExpectedSignature;
            header.SnapshotId = SnapshotId_;
            header.CompressedLength = Params_.CompressedLength;
            header.UncompressedLength = Params_.UncompressedLength;
            header.Checksum = Params_.Checksum;
            header.Codec = Codec_;
            header.MetaSize = SerializedMeta_.Size();
            File_->Seek(0, sSet);
            WritePod(*File_, header);
        }

        File_->Flush();
        File_->Close();

        NFS::Rename(FileName_ + TempFileSuffix, FileName_);

        Closed_.Fire();

        IsClosed_ = true;

        YT_LOG_DEBUG("Local snapshot writer closed");
    }

};

DEFINE_REFCOUNTED_TYPE(TFileSnapshotWriter)

ISnapshotWriterPtr CreateFileSnapshotWriter(
    const TString& fileName,
    ECodec codec,
    int snapshotId,
    const TSnapshotMeta& meta,
    bool raw)
{
    return New<TFileSnapshotWriter>(
        fileName,
        codec,
        snapshotId,
        meta,
        raw);
}

////////////////////////////////////////////////////////////////////////////////

class TFileSnapshotStore::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(TLocalSnapshotStoreConfigPtr config)
        : Config_(config)
    {
        Logger.AddTag("Path: %v", Config_->Path);
    }

    void Initialize()
    {
        auto path = Config_->Path;

        YT_LOG_DEBUG("Preparing snapshot directory");

        NFS::MakeDirRecursive(path);
        NFS::CleanTempFiles(path);

        YT_LOG_DEBUG("Looking for snapshots");

        auto fileNames = EnumerateFiles(path);
        for (const auto& fileName : fileNames) {
            auto extension = NFS::GetFileExtension(fileName);
            if (extension == SnapshotExtension) {
                auto name = NFS::GetFileNameWithoutExtension(fileName);
                try {
                    int snapshotId = FromString<int>(name);
                    RegisterSnapshot(snapshotId);
                } catch (const std::exception&) {
                    YT_LOG_WARNING("Found unrecognized file in snapshot directory (FileName: %v)", fileName);
                }
            }
        }

        YT_LOG_DEBUG("Snapshot scan complete");
    }

    bool CheckSnapshotExists(int snapshotId)
    {
        auto path = GetSnapshotPath(snapshotId);
        if (NFS::Exists(path)) {
            return true;
        }

        {
            auto guard = Guard(SpinLock_);
            if (SnapshotIds_.erase(snapshotId) == 1) {
                YT_LOG_WARNING("Erased orphaned snapshot %v from store", snapshotId);
            }
        }

        return false;
    }

    int GetLatestSnapshotId(int maxSnapshotId)
    {
        auto guard = Guard(SpinLock_);

        auto it = SnapshotIds_.upper_bound(maxSnapshotId);
        if (it == SnapshotIds_.begin()) {
            return InvalidSegmentId;
        }

        int snapshotId = *(--it);
        YT_VERIFY(snapshotId <= maxSnapshotId);
        return snapshotId;
    }

    ISnapshotReaderPtr CreateReader(int snapshotId)
    {
        if (!CheckSnapshotExists(snapshotId)) {
            THROW_ERROR_EXCEPTION("No such snapshot %v", snapshotId);
        }

        return CreateFileSnapshotReader(
            GetSnapshotPath(snapshotId),
            snapshotId,
            false);
    }

    ISnapshotReaderPtr CreateRawReader(int snapshotId, i64 offset)
    {
        return CreateFileSnapshotReader(
            GetSnapshotPath(snapshotId),
            snapshotId,
            true,
            offset);
    }

    ISnapshotWriterPtr CreateWriter(int snapshotId, const TSnapshotMeta& meta)
    {
        auto writer = New<TFileSnapshotWriter>(
            GetSnapshotPath(snapshotId),
            Config_->Codec,
            snapshotId,
            meta,
            false);
        RegisterWriter(writer, snapshotId);
        return writer;
    }

    ISnapshotWriterPtr CreateRawWriter(int snapshotId)
    {
        auto writer = New<TFileSnapshotWriter>(
            GetSnapshotPath(snapshotId),
            Config_->Codec,
            snapshotId,
            TSnapshotMeta(),
            true);
        RegisterWriter(writer, snapshotId);
        return writer;
    }

private:
    const TLocalSnapshotStoreConfigPtr Config_;

    NLogging::TLogger Logger = HydraLogger;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);
    std::set<int> SnapshotIds_;


    TString GetSnapshotPath(int snapshotId)
    {
        return NFS::CombinePaths(
            Config_->Path,
            Format("%09d.%v", snapshotId, SnapshotExtension));
    }

    void RegisterWriter(TFileSnapshotWriterPtr writer, int snapshotId)
    {
        writer->SubscribeClosed(BIND(
            &TImpl::RegisterSnapshot,
            MakeWeak(this),
            snapshotId));
    }

    void RegisterSnapshot(int snapshotId)
    {
        auto guard = Guard(SpinLock_);
        YT_VERIFY(SnapshotIds_.insert(snapshotId).second);
        YT_LOG_DEBUG("Registered snapshot %v", snapshotId);
    }

};

////////////////////////////////////////////////////////////////////////////////

TFileSnapshotStore::TFileSnapshotStore(TLocalSnapshotStoreConfigPtr config)
    : Impl_(New<TImpl>(config))
{ }

TFileSnapshotStore::~TFileSnapshotStore()
{ }

void TFileSnapshotStore::Initialize()
{
    Impl_->Initialize();
}

bool TFileSnapshotStore::CheckSnapshotExists(int snapshotId)
{
    return Impl_->CheckSnapshotExists(snapshotId);
}

int TFileSnapshotStore::GetLatestSnapshotId(int maxSnapshotId)
{
    return Impl_->GetLatestSnapshotId(maxSnapshotId);
}

ISnapshotReaderPtr TFileSnapshotStore::CreateReader(int snapshotId)
{
    return Impl_->CreateReader(snapshotId);
}

ISnapshotReaderPtr TFileSnapshotStore::CreateRawReader(int snapshotId, i64 offset)
{
    return Impl_->CreateRawReader(snapshotId, offset);
}

ISnapshotWriterPtr TFileSnapshotStore::CreateWriter(int snapshotId, const TSnapshotMeta& meta)
{
    return Impl_->CreateWriter(snapshotId, meta);
}

ISnapshotWriterPtr TFileSnapshotStore::CreateRawWriter(int snapshotId)
{
    return Impl_->CreateRawWriter(snapshotId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
