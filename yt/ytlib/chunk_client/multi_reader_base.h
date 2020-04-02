#pragma once

#include "public.h"

#include <yt/client/api/public.h>

#include <yt/client/chunk_client/data_statistics.h>
#include <yt/client/chunk_client/reader_base.h>

#include <yt/client/chunk_client/proto/chunk_spec.pb.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/concurrency/nonblocking_queue.h>

#include <yt/core/logging/log.h>

#include <yt/core/rpc/public.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TMultiReaderBase
    : public virtual IReaderBase
{
public:
    TMultiReaderBase(
        TMultiChunkReaderConfigPtr config,
        TMultiChunkReaderOptionsPtr options,
        const std::vector<IReaderFactoryPtr>& readerFactories,
        IMultiReaderMemoryManagerPtr multiReaderMemoryManager);

    void Open();

    virtual TFuture<void> GetReadyEvent() override;

    virtual NProto::TDataStatistics GetDataStatistics() const override;

    virtual TCodecStatistics GetDecompressionStatistics() const;

    virtual std::vector<TChunkId> GetFailedChunkIds() const override;

    virtual bool IsFetchingCompleted() const override;

protected:
    struct TSession
    {
        IReaderBasePtr Reader;
        int Index = -1;

        void Reset()
        {
            Reader.Reset();
            Index = -1;
        }
    };

    struct TUnreadState
    {
        IReaderBasePtr CurrentReader;
        std::vector<IReaderBasePtr> ActiveReaders;
        std::vector<IReaderFactoryPtr> ReaderFactories;
    };

    const TGuid Id_;
    const TMultiChunkReaderConfigPtr Config_;
    const TMultiChunkReaderOptionsPtr Options_;
    const std::vector<IReaderFactoryPtr> ReaderFactories_;
    const IMultiReaderMemoryManagerPtr MultiReaderMemoryManager_;

    const NLogging::TLogger Logger;

    TSession CurrentSession_;

    TFuture<void> ReadyEvent_;
    TPromise<void> CompletionError_ = NewPromise<void>();
    TFuture<void> UncancelableCompletionError_;

    IInvokerPtr ReaderInvoker_;

    virtual void OnReaderOpened(IReaderBasePtr chunkReader, int chunkIndex) = 0;

    virtual void OnReaderBlocked() = 0;

    virtual void OnReaderSwitched() = 0;

    virtual void OnReaderFinished();

    virtual void DoOpen() = 0;

    bool OnEmptyRead(bool readerFinished);

    void RegisterFailedReader(IReaderBasePtr reader);

protected:
    virtual void OnInterrupt();

    virtual TUnreadState GetUnreadState() const = 0;

    TSpinLock PrefetchLock_;
    int PrefetchIndex_ = 0;

    TSpinLock FailedChunksLock_;
    THashSet<TChunkId> FailedChunks_;

    std::atomic<int> OpenedReaderCount_ = { 0 };

    TSpinLock ActiveReadersLock_;
    NProto::TDataStatistics DataStatistics_;
    TCodecStatistics DecompressionStatistics_;
    std::atomic<int> ActiveReaderCount_ = { 0 };
    THashSet<IReaderBasePtr> ActiveReaders_;
    THashSet<int> NonOpenedReaderIndexes_;

    // If KeepInMemory option is set, we store here references to finished readers.
    std::vector<IReaderBasePtr> FinishedReaders_;

    TFuture<void> CombineCompletionError(TFuture<void> future);

    void OpenNextChunks();
    void DoOpenReader(int index);

};

////////////////////////////////////////////////////////////////////////////////

class TSequentialMultiReaderBase
    : public TMultiReaderBase
{
public:
    TSequentialMultiReaderBase(
        TMultiChunkReaderConfigPtr config,
        TMultiChunkReaderOptionsPtr options,
        const std::vector<IReaderFactoryPtr>& readerFactories,
        IMultiReaderMemoryManagerPtr multiReaderMemoryManager);

protected:
    virtual TUnreadState GetUnreadState() const override;

private:
    int NextReaderIndex_ = 0;
    int FinishedReaderCount_ = 0;
    std::vector<TPromise<IReaderBasePtr>> NextReaders_;

    virtual void DoOpen() override;

    virtual void OnReaderOpened(IReaderBasePtr chunkReader, int chunkIndex) override;

    virtual void OnReaderBlocked() override;

    virtual void OnReaderFinished() override;

    void WaitForNextReader();

    void WaitForCurrentReader();

    void PropagateError(const TError& error);

};

////////////////////////////////////////////////////////////////////////////////

class TParallelMultiReaderBase
    : public TMultiReaderBase
{
public:
    TParallelMultiReaderBase(
        TMultiChunkReaderConfigPtr config,
        TMultiChunkReaderOptionsPtr options,
        const std::vector<IReaderFactoryPtr>& readerFactories,
        IMultiReaderMemoryManagerPtr multiReaderMemoryManager);

protected:
    virtual TUnreadState GetUnreadState() const override;

private:
    typedef NConcurrency::TNonblockingQueue<TSession> TSessionQueue;

    TSessionQueue ReadySessions_;
    int FinishedReaderCount_ = 0;

    virtual void DoOpen() override;

    virtual void OnReaderOpened(IReaderBasePtr chunkReader, int chunkIndex) override;

    virtual void OnReaderBlocked() override;

    virtual void OnReaderFinished() override;

    void WaitForReadyReader();

    void WaitForReader(TSession session);

    void PropagateError(const TError& error);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
