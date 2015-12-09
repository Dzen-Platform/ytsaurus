#pragma once

#include "public.h"
#include "chunk_writer.h"

#include <yt/ytlib/chunk_client/chunk_info.pb.h>
#include <yt/ytlib/chunk_client/chunk_meta.pb.h>

#include <yt/core/misc/checksum.h>

#include <util/system/file.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! Provides a local and synchronous implementation of #IAsyncWriter.
class TFileWriter
    : public IChunkWriter
{
public:
    TFileWriter(
        const TChunkId& chunkId,
        const Stroka& fileName,
        bool syncOnClose = true);

    // IChunkWriter implementation.
    virtual TFuture<void> Open() override;

    virtual bool WriteBlock(const TSharedRef& block) override;
    virtual bool WriteBlocks(const std::vector<TSharedRef>& blocks) override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual TFuture<void> Close(const NChunkClient::NProto::TChunkMeta& chunkMeta) override;

    virtual const NChunkClient::NProto::TChunkInfo& GetChunkInfo() const override;
    virtual const NChunkClient::NProto::TDataStatistics& GetDataStatistics() const override;
    virtual TChunkReplicaList GetWrittenChunkReplicas() const override;

    virtual TChunkId GetChunkId() const override;

    virtual NErasure::ECodec GetErasureCodecId() const override;

    //! Returns the chunk meta.
    /*!
     *  The writer must be already closed.
     */
    const NChunkClient::NProto::TChunkMeta& GetChunkMeta() const;

    //! Aborts the writer, removing the temporary files.
    void Abort();

    //! Returns the total data size accumulated so far.
    /*!
     *  Can be called at any time.
     */
    i64 GetDataSize() const;

private:
    const TChunkId ChunkId_;
    const Stroka FileName_;
    const bool SyncOnClose_;

    bool IsOpen_ = false;
    bool IsClosed_ = false;
    i64 DataSize_ = 0;

    std::unique_ptr<TFile> DataFile_;

    NChunkClient::NProto::TChunkInfo ChunkInfo_;
    NChunkClient::NProto::TBlocksExt BlocksExt_;
    NChunkClient::NProto::TChunkMeta ChunkMeta_;

    TError Error_;

};

DEFINE_REFCOUNTED_TYPE(TFileWriter)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
