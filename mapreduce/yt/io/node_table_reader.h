#pragma once

#include <mapreduce/yt/interface/io.h>

#include <util/stream/input.h>
#include <util/generic/queue.h>
#include <util/generic/buffer.h>
#include <util/system/event.h>
#include <util/system/thread.h>
#include <util/system/spinlock.h>
#include <util/thread/lfqueue.h>

namespace NYT {

class TProxyInput;
class TRowBuilder;
class TYsonParser;

////////////////////////////////////////////////////////////////////////////////

struct TRowElement
    : public TThrRefBase
{
    TNode Node;
    size_t Size = 0;
    enum {
        ROW,
        ERROR,
        FINISH
    } Type = ROW;
};

using TRowElementPtr = TIntrusivePtr<TRowElement>;

////////////////////////////////////////////////////////////////////////////////

class TRowQueue
{
public:
    TRowQueue();

    void Enqueue(TRowElementPtr row);
    TRowElementPtr Dequeue();

    void Clear();
    void Stop();

private:
    yqueue<TRowElementPtr> Queue_;
    TSpinLock SpinLock_;
    TAtomic Size_;
    const size_t SizeLimit_;
    TAutoEvent EnqueueEvent_;
    TAutoEvent DequeueEvent_;
    volatile bool Stopped_;
};

////////////////////////////////////////////////////////////////////////////////

class TNodeTableReader
    : public INodeReaderImpl
{
public:
    explicit TNodeTableReader(THolder<TProxyInput> input);
    ~TNodeTableReader() override;

    const TNode& GetRow() const override;
    bool IsValid() const override;
    void Next() override;
    ui32 GetTableIndex() const override;
    ui64 GetRowIndex() const override;
    void NextKey() override;

private:
    THolder<TProxyInput> Input_;
    bool Valid_ = true;
    bool Finished_ = false;
    ui32 TableIndex_ = 0;
    TMaybe<ui64> RowIndex_;
    TMaybe<ui32> RangeIndex_;
    bool AtStart_ = true;

    TRowElementPtr Row_;
    TRowQueue RowQueue_;

    THolder<TRowBuilder> Builder_;
    THolder<TYsonParser> Parser_;

    yexception Exception_;

    volatile bool Running_;
    TAutoEvent RetryPrepared_;
    THolder<TThread> Thread_;

private:
    void OnStreamError();
    void CheckValidity() const;
    void PrepareParsing();

    void FetchThread();
    static void* FetchThread(void* opaque);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
