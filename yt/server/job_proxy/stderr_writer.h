#pragma once

#include <yt/core/misc/blob_output.h>

#include <yt/ytlib/file_client/file_chunk_output.h>

namespace NYT {
namespace NJobProxy {

class TTailBuffer
    : public TOutputStream
{
public:
    explicit TTailBuffer(i64 sizeLimit);

    bool IsOverflowed() const;
    void SaveTo(TOutputStream* out) const;

private:
    virtual void DoWrite(const void* buf, size_t len) override;

private:
    TBlob RingBuffer_;
    i64 Position_ = 0;
    bool BufferOverflowed_ = false;
};

////////////////////////////////////////////////////////////////////////////////

class TStderrWriter
    : public TOutputStream
{
public:
    TStderrWriter(
        NApi::TFileWriterConfigPtr config,
        NChunkClient::TMultiChunkWriterOptionsPtr options,
        NApi::IClientPtr client,
        const NObjectClient::TTransactionId& transactionId,
        size_t sizeLimit = std::numeric_limits<size_t>::max());

    NChunkClient::TChunkId GetChunkId() const;

    size_t GetCurrentSize() const;
    Stroka GetCurrentData() const;

private:
    virtual void DoWrite(const void* buf, size_t len) override;

    virtual void DoFinish() override;

    void SaveCurrentDataTo(TOutputStream* output) const;

private:
    NFileClient::TFileChunkOutput FileChunkOutput_;

    // Limit for the head or for the tail part.
    const size_t PartLimit_;

    TBlobOutput Head_;
    TNullable<TTailBuffer> Tail_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
