#pragma once

#include "chunk_reader.h"

#include <ytlib/chunk_client/chunk_meta.pb.h>

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
    explicit TFileReader(const Stroka& fileName);

    //! Opens the files, reads chunk meta. Must be called before reading blocks.
    void Open();

    //! Returns the meta file size.
    i64 GetMetaSize() const;

    //! Returns the data file size.
    i64 GetDataSize() const;

    //! Returns the full chunk size.
    i64 GetFullSize() const;

    //! Synchronously returns the requested meta.
    NProto::TChunkMeta GetMeta(const TNullable<std::vector<int>>& extensionTags = Null);

    // IReader implementation.
    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(const std::vector<int>& blockIndexes) override;

    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(int firstBlockIndex, int blockCount) override;
    
    virtual TFuture<NProto::TChunkMeta> GetMeta(
        const TNullable<int>& partitionTag,
        const TNullable<std::vector<int>>& extensionTags) override;

    virtual TChunkId GetChunkId() const override;

private:
    const Stroka FileName_;

    bool Opened_ = false;
    std::unique_ptr<TFile> DataFile_;
    
    i64 MetaSize_ = -1;
    i64 DataSize_ = -1;

    NChunkClient::NProto::TChunkMeta Meta_;
    NChunkClient::NProto::TBlocksExt BlocksExt_;
    int BlockCount_;


    TSharedRef ReadBlock(int blockIndex);

};

DEFINE_REFCOUNTED_TYPE(TFileReader)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
