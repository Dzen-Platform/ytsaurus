#pragma once

#include "public.h"

#include "data_statistics.h"
#include "multi_chunk_reader.h"

#include <ytlib/chunk_client/chunk_spec.pb.h>

#include <ytlib/node_tracker_client/public.h>

#include <core/concurrency/nonblocking_queue.h>
#include <core/concurrency/public.h>

#include <core/logging/log.h>

#include <core/rpc/public.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TMultiChunkReaderBase
    : public virtual IMultiChunkReader
{
public:
    TMultiChunkReaderBase(
        TMultiChunkReaderConfigPtr config,
        TMultiChunkReaderOptionsPtr options,
        NRpc::IChannelPtr masterChannel,
        IBlockCachePtr blockCache,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        const std::vector<NProto::TChunkSpec>& chunkSpecs,
        NConcurrency::IThroughputThrottlerPtr throttler);

    virtual TFuture<void> Open() override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual NProto::TDataStatistics GetDataStatistics() const override;

    virtual std::vector<TChunkId> GetFailedChunkIds() const override;

    virtual bool IsFetchingCompleted() const override;

protected:
    struct TSession
    {
        IChunkReaderBasePtr ChunkReader;
        int ChunkSpecIndex = -1;

        void Reset()
        {
            ChunkReader.Reset();
            ChunkSpecIndex = -1;
        }
    };

    NLogging::TLogger Logger;

    TMultiChunkReaderConfigPtr Config_;
    const TMultiChunkReaderOptionsPtr Options_;

    const std::vector<NProto::TChunkSpec> ChunkSpecs_;

    const NConcurrency::IThroughputThrottlerPtr Throttler_;

    TSession CurrentSession_;

    TFuture<void> ReadyEvent_;
    TPromise<void> CompletionError_;


    virtual void DoOpen() = 0;

    virtual IChunkReaderBasePtr CreateTemplateReader(
        const NProto::TChunkSpec& chunkSpec,
        IChunkReaderPtr asyncReader) = 0;

    virtual void OnReaderOpened(IChunkReaderBasePtr chunkReader, int chunkIndex) = 0;

    virtual void OnReaderBlocked() = 0;

    virtual void OnReaderSwitched() = 0;

    virtual void OnReaderFinished();

    virtual void OnError();

    bool OnEmptyRead(bool readerFinished);

    void OpenPrefetchChunks();

    void RegisterFailedChunk(int chunkIndex);

protected:
    const IBlockCachePtr BlockCache_;
    const NRpc::IChannelPtr MasterChannel_;
    const NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory_;

    int PrefetchReaderIndex_ = 0;
    int PrefetchWindow_;

    NConcurrency::TParallelAwaiterPtr FetchingCompletedAwaiter_;

    TSpinLock FailedChunksLock_;
    std::vector<TChunkId> FailedChunks_;

    bool IsOpen_ = false;

    int OpenedReaderCount_ = 0;

    TSpinLock DataStatisticsLock_;
    NProto::TDataStatistics DataStatistics_;
    yhash_set<IChunkReaderBasePtr> ActiveReaders_;

    // If KeepInMemory option is set, we store here references to finished readers.
    std::vector<IChunkReaderBasePtr> FinishedReaders_;


    IChunkReaderPtr CreateRemoteReader(const NProto::TChunkSpec& chunkSpec);

    void OpenNextChunk();
    void DoOpenNextChunk();

};

////////////////////////////////////////////////////////////////////////////////

class TSequentialMultiChunkReaderBase
    : public TMultiChunkReaderBase
{
public:
    TSequentialMultiChunkReaderBase(
        TMultiChunkReaderConfigPtr config,
        TMultiChunkReaderOptionsPtr options,
        NRpc::IChannelPtr masterChannel,
        IBlockCachePtr blockCache,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        const std::vector<NProto::TChunkSpec>& chunkSpecs,
        NConcurrency::IThroughputThrottlerPtr throttler);

private:
    int NextReaderIndex_ = 0;
    std::vector<TPromise<IChunkReaderBasePtr>> NextReaders_;


    virtual void DoOpen() override;

    virtual void OnReaderOpened(IChunkReaderBasePtr chunkReader, int chunkIndex) override;

    virtual void OnReaderBlocked() override;

    virtual void OnReaderFinished() override; 

    virtual void OnError() override;

    void WaitForNextReader();

    void WaitForCurrentReader();

};

////////////////////////////////////////////////////////////////////////////////

class TParallelMultiChunkReaderBase
    : public TMultiChunkReaderBase
{
public:
    TParallelMultiChunkReaderBase(
        TMultiChunkReaderConfigPtr config,
        TMultiChunkReaderOptionsPtr options,
        NRpc::IChannelPtr masterChannel,
        IBlockCachePtr blockCache,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        const std::vector<NProto::TChunkSpec>& chunkSpecs,
        NConcurrency::IThroughputThrottlerPtr throttler);

private:
    typedef NConcurrency::TNonblockingQueue<TSession> TSessionQueue;

    TSessionQueue ReadySessions_;
    int FinishedReaderCount_ = 0;


    virtual void DoOpen() override;

    virtual void OnReaderOpened(IChunkReaderBasePtr chunkReader, int chunkIndex) override;

    virtual void OnReaderBlocked() override;

    virtual void OnReaderFinished() override;

    virtual void OnError() override;

    void WaitForReadyReader();

    void WaitForReader(TSession session);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
