#include "file_reader.h"
#include "chunk_meta_extensions.h"
#include "format.h"

#include <yt/core/misc/fs.h>
#include <yt/core/misc/checksum.h>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;

///////////////////////////////////////////////////////////////////////////////

struct TFileReaderDataBufferTag
{ };

struct TFileReaderMetaBufferTag
{ };

namespace {

template <class T>
void ReadHeader(
    const TSharedMutableRef& metaFileBlob,
    const Stroka& fileName,
    TChunkMetaHeader_2* metaHeader,
    TRef* metaBlob)
{
    if (metaFileBlob.Size() < sizeof(T)) {
        THROW_ERROR_EXCEPTION("Chunk meta file %v is too short: at least %v bytes expected",
            fileName,
            sizeof(T));
    }
    *static_cast<T*>(metaHeader) = *reinterpret_cast<const T*>(metaFileBlob.Begin());
    *metaBlob = metaFileBlob.Slice(sizeof(T), metaFileBlob.Size());
}

} // namespace

TFileReader::TFileReader(
    const TChunkId& chunkId,
    const Stroka& fileName,
    bool validateBlocksChecksums)
    : ChunkId_(chunkId)
    , FileName_(fileName)
    , ValidateBlockChecksums_(validateBlocksChecksums)
{ }


TFuture<std::vector<TSharedRef>> TFileReader::ReadBlocks(
    const TWorkloadDescriptor& /*workloadDescriptor*/,
    const std::vector<int>& blockIndexes)
{
    std::vector<TSharedRef> blocks;
    blocks.reserve(blockIndexes.size());

    try {
        NFS::ExpectIOErrors([&] () {
            // Extract maximum contiguous ranges of blocks.
            int localIndex = 0;
            while (localIndex < blockIndexes.size()) {
                int startLocalIndex = localIndex;
                int startBlockIndex = blockIndexes[startLocalIndex];
                int endLocalIndex = startLocalIndex;
                while (endLocalIndex < blockIndexes.size() &&
                       blockIndexes[endLocalIndex] == startBlockIndex + (endLocalIndex - startLocalIndex))
                {
                    ++endLocalIndex;
                }

                int blockCount = endLocalIndex - startLocalIndex;
                auto subblocks = DoReadBlocks(startBlockIndex, blockCount);
                blocks.insert(blocks.end(), subblocks.begin(), subblocks.end());

                localIndex = endLocalIndex;
            }
        });
    } catch (const std::exception& ex) {
        return MakeFuture<std::vector<TSharedRef>>(ex);
    }

    return MakeFuture(std::move(blocks));
}

TFuture<std::vector<TSharedRef>> TFileReader::ReadBlocks(
    const TWorkloadDescriptor& /*workloadDescriptor*/,
    int firstBlockIndex,
    int blockCount)
{
    YCHECK(firstBlockIndex >= 0);

    std::vector<TSharedRef> blocks;

    try {
        NFS::ExpectIOErrors([&] () {
            blocks = DoReadBlocks(firstBlockIndex, blockCount);
        });
    } catch (const std::exception& ex) {
        return MakeFuture<std::vector<TSharedRef>>(ex);
    }

    return MakeFuture(std::move(blocks));
}

TFuture<TChunkMeta> TFileReader::GetMeta(
    const TWorkloadDescriptor& /*workloadDescriptor*/,
    const TNullable<int>& partitionTag,
    const TNullable<std::vector<int>>& extensionTags)
{
    TChunkMeta meta;

    try {
        NFS::ExpectIOErrors([&] () {
            meta = DoGetMeta(partitionTag, extensionTags);
        });
    } catch (const std::exception& ex) {
        return MakeFuture<TChunkMeta>(ex);
    }

    return MakeFuture(std::move(meta));
}

TChunkId TFileReader::GetChunkId() const
{
    return ChunkId_;
}

std::vector<TSharedRef> TFileReader::DoReadBlocks(
    int firstBlockIndex,
    int blockCount)
{
    const auto& blockExts = GetBlockExts();
    int chunkBlockCount = blockExts.blocks_size();
    if (firstBlockIndex + blockCount > chunkBlockCount) {
        THROW_ERROR_EXCEPTION("Requested to read blocks [%v,%v] from chunk %v while only %v blocks exist",
            firstBlockIndex,
            firstBlockIndex + blockCount - 1,
            FileName_,
            chunkBlockCount);
    }

    // Read all blocks within a single request.
    int lastBlockIndex = firstBlockIndex + blockCount - 1;
    const auto& firstBlockInfo = blockExts.blocks(firstBlockIndex);
    const auto& lastBlockInfo = blockExts.blocks(lastBlockIndex);
    i64 totalSize = lastBlockInfo.offset() + lastBlockInfo.size() - firstBlockInfo.offset();

    auto data = TSharedMutableRef::Allocate<TFileReaderDataBufferTag>(totalSize, false);

    auto& file = GetDataFile();
    file.Pread(data.Begin(), data.Size(), firstBlockInfo.offset());

    // Slice the result; validate checksums.
    std::vector<TSharedRef> blocks;
    blocks.reserve(blockCount);
    for (int localIndex = 0; localIndex < blockCount; ++localIndex) {
        int blockIndex = firstBlockIndex + localIndex;
        const auto& blockInfo = blockExts.blocks(blockIndex);
        auto block = data.Slice(
            blockInfo.offset() - firstBlockInfo.offset(),
            blockInfo.offset() - firstBlockInfo.offset() + blockInfo.size());
        if (ValidateBlockChecksums_) {
            auto checksum = GetChecksum(block);
            if (checksum != blockInfo.checksum()) {
                THROW_ERROR_EXCEPTION("Incorrect checksum of block %v in chunk data file %Qv: expected %v, actual %v",
                      blockIndex,
                      FileName_,
                      blockInfo.checksum(),
                      checksum);
            }
        }
        blocks.push_back(block);
    }

    return blocks;
}

TChunkMeta TFileReader::DoGetMeta(
    const TNullable<int>& partitionTag,
    const TNullable<std::vector<int>>& extensionTags)
{
    // Partition tag filtering not implemented here
    // because there is no practical need.
    // Implement when necessary.
    YCHECK(!partitionTag);

    auto metaFileName = FileName_ + ChunkMetaSuffix;
    TFile metaFile(
        metaFileName,
        OpenExisting | RdOnly | Seq | CloseOnExec);

    if (metaFile.GetLength() < sizeof (TChunkMetaHeaderBase)) {
        THROW_ERROR_EXCEPTION("Chunk meta file %v is too short: at least %v bytes expected",
            FileName_,
            sizeof (TChunkMetaHeaderBase));
    }

    auto metaFileBlob = TSharedMutableRef::Allocate<TFileReaderMetaBufferTag>(metaFile.GetLength());

    TBufferedFileInput metaFileInput(metaFile);
    metaFileInput.Read(metaFileBlob.Begin(), metaFile.GetLength());

    TChunkMetaHeader_2 metaHeader;
    TRef metaBlob;
    const auto* metaHeaderBase = reinterpret_cast<const TChunkMetaHeaderBase*>(metaFileBlob.Begin());

    switch (metaHeaderBase->Signature) {
        case TChunkMetaHeader_1::ExpectedSignature:
            ReadHeader<TChunkMetaHeader_1>(metaFileBlob, metaFileName, &metaHeader, &metaBlob);
            metaHeader.ChunkId = ChunkId_;
            break;

        case TChunkMetaHeader_2::ExpectedSignature:
            ReadHeader<TChunkMetaHeader_2>(metaFileBlob, metaFileName, &metaHeader, &metaBlob);
            break;

        default:
            THROW_ERROR_EXCEPTION("Incorrect header signature %x in chunk meta file %v",
                metaHeaderBase->Signature,
                FileName_);
    }

    auto checksum = GetChecksum(metaBlob);
    if (checksum != metaHeader.Checksum) {
        THROW_ERROR_EXCEPTION("Incorrect checksum in chunk meta file %v: expected %v, actual %v",
            metaFileName,
            metaHeader.Checksum,
            checksum);
    }

    if (ChunkId_ != NullChunkId && metaHeader.ChunkId != ChunkId_) {
        THROW_ERROR_EXCEPTION("Invalid chunk id in meta file %v: expected %v, actual %v",
            metaFileName,
            ChunkId_,
            metaHeader.ChunkId);
    }

    TChunkMeta meta;
    if (!TryDeserializeFromProtoWithEnvelope(&meta, metaBlob)) {
        THROW_ERROR_EXCEPTION("Failed to parse chunk meta file %v",
            metaFileName);
    }

    return meta;
}

const TBlocksExt& TFileReader::GetBlockExts()
{
    if (!HasCachedBlocksExt_) {
        TGuard<TMutex> guard(Mutex_);
        if (!CachedBlocksExt_) {
            auto meta = DoGetMeta(Null, Null);
            CachedBlocksExt_ = GetProtoExtension<TBlocksExt>(meta.extensions());
            HasCachedBlocksExt_ = true;
        }
    }
    return *CachedBlocksExt_;
}

TFile& TFileReader::GetDataFile()
{
    if (!HasCachedDataFile_) {
        TGuard<TMutex> guard(Mutex_);
        if (!CachedDataFile_) {
            CachedDataFile_.reset(new TFile(FileName_, OpenExisting | RdOnly | CloseOnExec));
            HasCachedDataFile_ = true;
        }
    }
    return *CachedDataFile_;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
