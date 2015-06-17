#pragma once

#include "public.h"

#include "chunk_replica.h"
#include "data_statistics.h"
#include "multi_chunk_writer.h"

#include <ytlib/node_tracker_client/public.h>

#include <ytlib/transaction_client/public.h>

#include <core/concurrency/parallel_awaiter.h>

#include <core/logging/log.h>

#include <core/rpc/public.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TNontemplateMultiChunkWriterBase
    : public virtual IMultiChunkWriter
{
public:
    TNontemplateMultiChunkWriterBase(
        TMultiChunkWriterConfigPtr config,
        TMultiChunkWriterOptionsPtr options,
        NRpc::IChannelPtr masterChannel,
        const NTransactionClient::TTransactionId& transactionId,
        const TChunkListId& parentChunkListId,
        NConcurrency::IThroughputThrottlerPtr throttler,
        IBlockCachePtr blockCache);

    virtual TFuture<void> Open() override;
    virtual TFuture<void> Close() override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual void SetProgress(double progress) override;

    /*!
     *  To get consistent data, should be called only when the writer is closed.
     */
    virtual const std::vector<NProto::TChunkSpec>& GetWrittenChunks() const override    ;

    //! Provides node id to descriptor mapping for chunks returned via #GetWrittenChunks.
    virtual NNodeTrackerClient::TNodeDirectoryPtr GetNodeDirectory() const override;

    virtual NProto::TDataStatistics GetDataStatistics() const override;

protected:
    NLogging::TLogger Logger;

    bool VerifyActive();
    bool TrySwitchSession();

    virtual IChunkWriterBasePtr CreateTemplateWriter(IChunkWriterPtr underlyingWriter) = 0;

private:
    struct TSession
    {
        IChunkWriterBasePtr TemplateWriter;
        IChunkWriterPtr UnderlyingWriter;
        TChunkId ChunkId;

        bool IsActive() const
        {
            return bool(TemplateWriter);
        }

        void Reset()
        {
            TemplateWriter.Reset();
            UnderlyingWriter.Reset();
            ChunkId = TChunkId();
        }
    };

    const TMultiChunkWriterConfigPtr Config_;
    const TMultiChunkWriterOptionsPtr Options_;
    const NRpc::IChannelPtr MasterChannel_;
    const NTransactionClient::TTransactionId TransactionId_;
    const TChunkListId ParentChunkListId_;
    const NConcurrency::IThroughputThrottlerPtr Throttler_;
    const IBlockCachePtr BlockCache_;

    const NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory_;
    const NConcurrency::TParallelAwaiterPtr CloseChunksAwaiter_;

    volatile double Progress_ = 0.0;

    TSession CurrentSession_;
    TSession NextSession_;

    bool Closing_ = false;

    TFuture<void> NextSessionReady_;
    TFuture<void> ReadyEvent_ = VoidFuture;

    TPromise<void> CompletionError_ = NewPromise<void>();

    NProto::TDataStatistics DataStatistics_;
    std::vector<NChunkClient::NProto::TChunkSpec> WrittenChunks_;


    void DoOpen();
    void DoClose();

    void CreateNextSession();
    void InitCurrentSession();

    void SwitchSession();
    void DoSwitchSession(const TSession& session);

    TFuture<void> FinishSession(const TSession& session);
    void DoFinishSession(const TSession& session);

};

////////////////////////////////////////////////////////////////////////////////

template <class IMultiChunkWriter, class ISpecificChunkWriter, class... TWriteArgs>
class TMultiChunkWriterBase
    : public TNontemplateMultiChunkWriterBase
    , public IMultiChunkWriter
{
public:
    typedef TIntrusivePtr<ISpecificChunkWriter> ISpecificChunkWriterPtr;

    TMultiChunkWriterBase(
        TMultiChunkWriterConfigPtr config,
        TMultiChunkWriterOptionsPtr options,
        NRpc::IChannelPtr masterChannel,
        const NTransactionClient::TTransactionId& transactionId,
        const TChunkListId& parentChunkListId,
        std::function<ISpecificChunkWriterPtr(IChunkWriterPtr)> createChunkWriter,
        NConcurrency::IThroughputThrottlerPtr throttler,
        IBlockCachePtr blockCache)
        : TNontemplateMultiChunkWriterBase(
            config, 
            options, 
            masterChannel, 
            transactionId, 
            parentChunkListId,
            throttler,
            blockCache)
        , CreateChunkWriter_(createChunkWriter)
    { }

    virtual bool Write(TWriteArgs... args) override
    {
        if (!VerifyActive()) {
            return false;
        }

        // Return true if current writer is ready for more data and
        // we didn't switch to the next chunk.
        bool readyForMore = CurrentWriter_->Write(std::forward<TWriteArgs>(args)...);
        bool switched = false;
        if (readyForMore) {
            switched = TrySwitchSession();
        }
        return readyForMore && !switched;
    }

protected:
    const std::function<ISpecificChunkWriterPtr(IChunkWriterPtr)> CreateChunkWriter_;

    ISpecificChunkWriterPtr CurrentWriter_;

    virtual IChunkWriterBasePtr CreateTemplateWriter(IChunkWriterPtr underlyingWriter) override
    {
        CurrentWriter_ = CreateChunkWriter_(underlyingWriter);
        return CurrentWriter_;
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
