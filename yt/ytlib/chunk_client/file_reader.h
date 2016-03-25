#pragma once

#include "chunk_reader.h"

#include <yt/ytlib/chunk_client/chunk_meta.pb.h>

#include <util/system/file.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! Provides a local and synchronous implementation of IReader.
class TFileReader
    : public IChunkReader
{
public:
    //! Creates a new reader.
    /*!
     *  For chunk meta version 2+, #chunkId is validated against that stored
     *  in the meta file. Passing #NullChunkId in #chunkId suppresses this check.
     */
    TFileReader(
        const TChunkId& chunkId,
        const Stroka& fileName,
        bool validateBlocksChecksums = true);


    // IReader implementation.
    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        const std::vector<int>& blockIndexes) override;

    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        int firstBlockIndex,
        int blockCount) override;

    virtual TFuture<NProto::TChunkMeta> GetMeta(
        const TWorkloadDescriptor& workloadDescriptor,
        const TNullable<int>& partitionTag = Null,
        const TNullable<std::vector<int>>& extensionTags = Null) override;

    virtual TChunkId GetChunkId() const override;

private:
    const TChunkId ChunkId_;
    const Stroka FileName_;
    const bool ValidateBlockChecksums_;

    std::unique_ptr<TFile> CachedDataFile_;
    TNullable<NProto::TBlocksExt> CachedBlocksExt_;

    std::vector<TSharedRef> DoReadBlocks(int firstBlockIndex, int blockCount);
    NProto::TChunkMeta DoGetMeta(
        const TNullable<int>& partitionTag,
        const TNullable<std::vector<int>>& extensionTags);

    const NProto::TBlocksExt& GetBlockExts();
    TFile& GetDataFile();
};

DEFINE_REFCOUNTED_TYPE(TFileReader)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
