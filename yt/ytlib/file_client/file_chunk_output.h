#pragma once

#include "public.h"
#include "chunk_meta_extensions.h"

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/chunk_client/chunk_replica.h>
#include <yt/ytlib/chunk_client/public.h>

#include <yt/core/compression/codec.h>

#include <yt/core/logging/log.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NFileClient {

////////////////////////////////////////////////////////////////////////////////

//! A client-side facade for writing files.
/*!
 *  The client must call #Open and then feed the data in by calling #Write.
 *  Finally it must call #Finish.
 */
class TFileChunkOutput
    : public TOutputStream
{
public:
    //! Initializes an instance.
    TFileChunkOutput(
        NApi::TFileWriterConfigPtr config,
        NChunkClient::TMultiChunkWriterOptionsPtr options,
        NApi::IClientPtr client,
        const NObjectClient::TTransactionId& transactionId,
        i64 sizeLimit = std::numeric_limits<i64>::max());

    NChunkClient::TChunkId GetChunkId() const;
    i64 GetSize() const;

protected:
    //! Adds another portion of data.
    /*!
     *  This portion does not necessary makes up a block. The writer maintains an internal buffer
     *  and splits the input data into parts of equal size (see #TConfig::BlockSize).
     */
    virtual void DoWrite(const void* buf, size_t len) override;

    //! Closes the writer.
    virtual void DoFinish() override;

private:
    void FlushBlock();

    const NApi::TFileWriterConfigPtr Config_;
    const NChunkClient::TMultiChunkWriterOptionsPtr Options_;

    NApi::IClientPtr Client_;
    NObjectClient::TTransactionId TransactionId_;

    NChunkClient::IChunkWriterPtr ConfirmingChunkWriter_;
    IFileChunkWriterPtr FileChunkWriter_;

    const i64 SizeLimit_;

protected:
    NLogging::TLogger Logger;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
