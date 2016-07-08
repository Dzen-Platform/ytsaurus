#include "async_stream.h"
#include "scheduler.h"

#include <queue>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

namespace {

template <class T>
TErrorOr<T> WaitForWithStrategy(
    TFuture<T>&& future,
    ESyncStreamAdapterStrategy strategy)
{
    switch (strategy) {
        case ESyncStreamAdapterStrategy::WaitFor:
            return WaitFor(std::move(future));
        case ESyncStreamAdapterStrategy::Get:
            return future.Get();
        default:
            Y_UNREACHABLE();
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TSyncInputStreamAdapter
    : public TInputStream
{
public:
    TSyncInputStreamAdapter(
        IAsyncInputStreamPtr underlyingStream,
        ESyncStreamAdapterStrategy strategy)
        : UnderlyingStream_(std::move(underlyingStream))
        , Strategy_(strategy)
    { }

private:
    const IAsyncInputStreamPtr UnderlyingStream_;
    const ESyncStreamAdapterStrategy Strategy_;

    virtual size_t DoRead(void* buffer, size_t length) override
    {
        auto future = UnderlyingStream_->Read(TSharedMutableRef(buffer, length, nullptr));
        return WaitForWithStrategy(std::move(future), Strategy_)
            .ValueOrThrow();
    }
};

std::unique_ptr<TInputStream> CreateSyncAdapter(
    IAsyncInputStreamPtr underlyingStream,
    ESyncStreamAdapterStrategy strategy)
{
    YCHECK(underlyingStream);
    return std::make_unique<TSyncInputStreamAdapter>(
        std::move(underlyingStream),
        strategy);
}

////////////////////////////////////////////////////////////////////////////////

class TAsyncInputStreamAdapter
    : public IAsyncInputStream
{
public:
    TAsyncInputStreamAdapter(
        TInputStream* underlyingStream,
        IInvokerPtr invoker)
        : UnderlyingStream_(underlyingStream)
        , Invoker_(std::move(invoker))
    { }

    virtual TFuture<size_t> Read(const TSharedMutableRef& buffer) override
    {
        return
            BIND(&TAsyncInputStreamAdapter::DoRead, MakeStrong(this), buffer)
            .AsyncVia(Invoker_)
            .Run();
    }

private:
    size_t DoRead(const TSharedMutableRef& buffer) const
    {
        return UnderlyingStream_->Read(buffer.Begin(), buffer.Size());
    }

private:
    TInputStream* const UnderlyingStream_;
    const IInvokerPtr Invoker_;
};

IAsyncInputStreamPtr CreateAsyncAdapter(
    TInputStream* underlyingStream,
    IInvokerPtr invoker)
{
    YCHECK(underlyingStream);
    return New<TAsyncInputStreamAdapter>(underlyingStream, std::move(invoker));
}

////////////////////////////////////////////////////////////////////////////////

class TSyncOutputStreamAdapter
    : public TOutputStream
{
public:
    TSyncOutputStreamAdapter(
        IAsyncOutputStreamPtr underlyingStream,
        ESyncStreamAdapterStrategy strategy)
        : UnderlyingStream_(std::move(underlyingStream))
        , Strategy_(strategy)
    { }

    virtual ~TSyncOutputStreamAdapter() throw()
    { }

private:
    const IAsyncOutputStreamPtr UnderlyingStream_;
    const ESyncStreamAdapterStrategy Strategy_;

    virtual void DoWrite(const void* buffer, size_t length) override
    {
        auto future = UnderlyingStream_->Write(TSharedRef(buffer, length, nullptr));
        WaitForWithStrategy(std::move(future), Strategy_)
            .ThrowOnError();
    }
};

std::unique_ptr<TOutputStream> CreateSyncAdapter(
    IAsyncOutputStreamPtr underlyingStream,
    ESyncStreamAdapterStrategy strategy)
{
    YCHECK(underlyingStream);
    return std::make_unique<TSyncOutputStreamAdapter>(
        std::move(underlyingStream),
        strategy);
}

////////////////////////////////////////////////////////////////////////////////

class TAsyncOutputStreamAdapter
    : public IAsyncOutputStream
{
public:
    TAsyncOutputStreamAdapter(
        TOutputStream* underlyingStream,
        IInvokerPtr invoker)
        : UnderlyingStream_(underlyingStream)
        , Invoker_(std::move(invoker))
    { }

    virtual TFuture<void> Write(const TSharedRef& buffer) override
    {
        return
            BIND(&TAsyncOutputStreamAdapter::DoWrite, MakeStrong(this), buffer)
            .AsyncVia(Invoker_)
            .Run();
    }

private:
    void DoWrite(const TSharedRef& buffer) const
    {
        UnderlyingStream_->Write(buffer.Begin(), buffer.Size());
    }

private:
    TOutputStream* const UnderlyingStream_;
    const IInvokerPtr Invoker_;
};

IAsyncOutputStreamPtr CreateAsyncAdapter(
    TOutputStream* underlyingStream,
    IInvokerPtr invoker)
{
    YCHECK(underlyingStream);
    return New<TAsyncOutputStreamAdapter>(underlyingStream, std::move(invoker));
}

////////////////////////////////////////////////////////////////////////////////

class TZeroCopyInputStreamAdapter
    : public IAsyncZeroCopyInputStream
{
public:
    TZeroCopyInputStreamAdapter(
        IAsyncInputStreamPtr underlyingStream,
        size_t blockSize)
        : UnderlyingStream_(std::move(underlyingStream))
        , BlockSize_(blockSize)
    { }

    virtual TFuture<TSharedRef> Read() override
    {
        struct TZeroCopyInputStreamAdapterBlockTag
        { };

        auto promise = NewPromise<TSharedRef>();
        auto block = TSharedMutableRef::Allocate<TZeroCopyInputStreamAdapterBlockTag>(BlockSize_, false);

        DoRead(promise, std::move(block), 0);

        return promise;
    }

private:
    const IAsyncInputStreamPtr UnderlyingStream_;
    const size_t BlockSize_;


    void DoRead(
        TPromise<TSharedRef> promise,
        TSharedMutableRef block,
        size_t offset)
    {
        if (block.Size() == offset) {
            promise.Set(std::move(block));
            return;
        }

        UnderlyingStream_->Read(block.Slice(offset, block.Size())).Subscribe(
            BIND(
                &TZeroCopyInputStreamAdapter::OnRead,
                MakeStrong(this),
                std::move(promise),
                std::move(block),
                offset));
    }

    void OnRead(
        TPromise<TSharedRef> promise,
        TSharedMutableRef block,
        size_t offset,
        const TErrorOr<size_t>& result)
    {
        if (!result.IsOK()) {
            promise.Set(TError(result));
            return;
        }

        auto bytes = result.Value();

        if (bytes == 0) {
            promise.Set(offset == 0 ? TSharedRef() : block.Slice(0, offset));
            return;
        }

        DoRead(std::move(promise), std::move(block), offset + bytes);
    }
};

IAsyncZeroCopyInputStreamPtr CreateZeroCopyAdapter(
    IAsyncInputStreamPtr underlyingStream,
    size_t blockSize)
{
    YCHECK(underlyingStream);
    return New<TZeroCopyInputStreamAdapter>(underlyingStream, blockSize);
}

////////////////////////////////////////////////////////////////////////////////

class TCopyingInputStreamAdapter
    : public IAsyncInputStream
{
public:
    explicit TCopyingInputStreamAdapter(IAsyncZeroCopyInputStreamPtr underlyingStream)
        : UnderlyingStream_(underlyingStream)
    {
        YCHECK(UnderlyingStream_);
    }

    virtual TFuture<size_t> Read(const TSharedMutableRef& buffer) override
    {
        if (CurrentBlock_) {
            // NB(psushin): no swapping here, it's a _copying_ adapter!
            // Also, #buffer may be constructed via FromNonOwningRef.
            return MakeFuture<size_t>(DoCopy(buffer));
        } else {
            return UnderlyingStream_->Read().Apply(
                BIND(&TCopyingInputStreamAdapter::OnRead, MakeStrong(this), buffer));
        }
    }

private:
    const IAsyncZeroCopyInputStreamPtr UnderlyingStream_;

    TSharedRef CurrentBlock_;
    i64 CurrentOffset_ = 0;


    size_t DoCopy(const TMutableRef& buffer)
    {
        size_t remaining = CurrentBlock_.Size() - CurrentOffset_;
        size_t bytes = std::min(buffer.Size(), remaining);
        ::memcpy(buffer.Begin(), CurrentBlock_.Begin() + CurrentOffset_, bytes);
        CurrentOffset_ += bytes;
        if (CurrentOffset_ == CurrentBlock_.Size()) {
            CurrentBlock_.Reset();
            CurrentOffset_ = 0;
        }
        return bytes;
    }

    size_t OnRead(const TSharedMutableRef& buffer, const TSharedRef& block)
    {
        CurrentBlock_ = block;
        return DoCopy(buffer);
    }

};

IAsyncInputStreamPtr CreateCopyingAdapter(IAsyncZeroCopyInputStreamPtr underlyingStream)
{
    return New<TCopyingInputStreamAdapter>(underlyingStream);
}

////////////////////////////////////////////////////////////////////////////////

class TZeroCopyOutputStreamAdapter
    : public IAsyncZeroCopyOutputStream
{
public:
    explicit TZeroCopyOutputStreamAdapter(IAsyncOutputStreamPtr underlyingStream)
        : UnderlyingStream_(underlyingStream)
    {
        YCHECK(UnderlyingStream_);
    }

    virtual TFuture<void> Write(const TSharedRef& data) override
    {
        Y_ASSERT(data);
        TPromise<void> promise;
        bool invokeWrite;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (!Error_.IsOK()) {
                return MakeFuture(Error_);
            }
            promise = NewPromise<void>();
            Queue_.push(TEntry{data, promise});
            invokeWrite = (Queue_.size() == 1);
        }
        if (invokeWrite) {
            UnderlyingStream_->Write(data).Subscribe(
                BIND(&TZeroCopyOutputStreamAdapter::OnWritten, MakeStrong(this)));
        }
        return promise;
    }

private:
    const IAsyncOutputStreamPtr UnderlyingStream_;

    struct TEntry
    {
        TSharedRef Block;
        TPromise<void> Promise;
    };

    TSpinLock SpinLock_;
    std::queue<TEntry> Queue_;
    TError Error_;


    void OnWritten(const TError& error)
    {
        auto pendingBlock = NotifyAndFetchNext(error);
        while (pendingBlock) {
            auto asyncWriteResult = UnderlyingStream_->Write(pendingBlock);
            auto mayWriteResult = asyncWriteResult.TryGet();
            if (!mayWriteResult || !mayWriteResult->IsOK()) {
                asyncWriteResult.Subscribe(
                    BIND(&TZeroCopyOutputStreamAdapter::OnWritten, MakeStrong(this)));
                break;
            }
            pendingBlock = NotifyAndFetchNext(TError());
        }
    }

    TSharedRef NotifyAndFetchNext(const TError& error)
    {
        TPromise<void> promise;
        TSharedRef pendingBlock;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            auto& entry = Queue_.front();
            promise = std::move(entry.Promise);
            if (!error.IsOK() && Error_.IsOK()) {
                Error_ = error;
            }
            Queue_.pop();
            if (!Queue_.empty()) {
                pendingBlock = Queue_.front().Block;
            }
        }
        promise.Set(error);
        return pendingBlock;
    }

};

IAsyncZeroCopyOutputStreamPtr CreateZeroCopyAdapter(IAsyncOutputStreamPtr underlyingStream)
{
    return New<TZeroCopyOutputStreamAdapter>(underlyingStream);
}

////////////////////////////////////////////////////////////////////////////////

class TCopyingOutputStreamAdapter
    : public IAsyncOutputStream
{
public:
    explicit TCopyingOutputStreamAdapter(IAsyncZeroCopyOutputStreamPtr underlyingStream)
        : UnderlyingStream_(underlyingStream)
    {
        YCHECK(UnderlyingStream_);
    }

    virtual TFuture<void> Write(const TSharedRef& buffer) override
    {
        struct TCopyingOutputStreamAdapterBlockTag { };
        auto block = TSharedMutableRef::Allocate<TCopyingOutputStreamAdapterBlockTag>(buffer.Size(), false);
        ::memcpy(block.Begin(), buffer.Begin(), buffer.Size());
        return UnderlyingStream_->Write(block);
    }

private:
    const IAsyncZeroCopyOutputStreamPtr UnderlyingStream_;

};

IAsyncOutputStreamPtr CreateCopyingAdapter(IAsyncZeroCopyOutputStreamPtr underlyingStream)
{
    return New<TCopyingOutputStreamAdapter>(underlyingStream);
}

////////////////////////////////////////////////////////////////////////////////

class TPrefetchingInputStreamAdapter
    : public IAsyncZeroCopyInputStream
{
public:
    TPrefetchingInputStreamAdapter(
        IAsyncZeroCopyInputStreamPtr underlyingStream,
        size_t windowSize)
        : UnderlyingStream_(underlyingStream)
        , WindowSize_(windowSize)
    {
        YCHECK(UnderlyingStream_);
        YCHECK(WindowSize_ > 0);
    }

    virtual TFuture<TSharedRef> Read() override
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (!Error_.IsOK()) {
            return MakeFuture<TSharedRef>(Error_);
        }
        if (PrefetchedBlocks_.empty()) {
            return Prefetch(&guard).Apply(
                BIND(&TPrefetchingInputStreamAdapter::OnPrefetched, MakeStrong(this)));
        }
        return MakeFuture<TSharedRef>(PopBlock(&guard));
    }

private:
    const IAsyncZeroCopyInputStreamPtr UnderlyingStream_;
    const size_t WindowSize_;

    TSpinLock SpinLock_;
    TError Error_;
    std::queue<TSharedRef> PrefetchedBlocks_;
    size_t PrefetchedSize_ = 0;
    TFuture<void> OutstandingResult_;


    TFuture<void> Prefetch(TGuard<TSpinLock>* guard)
    {
        if (OutstandingResult_) {
            guard->Release();
            return OutstandingResult_;
        }
        auto promise = NewPromise<void>();
        OutstandingResult_ = promise;
        guard->Release();
        UnderlyingStream_->Read().Subscribe(BIND(
            &TPrefetchingInputStreamAdapter::OnRead,
            MakeStrong(this),
            promise));
        return promise;
    }

    void OnRead(TPromise<void> promise, const TErrorOr<TSharedRef>& result)
    {
        {
            TGuard<TSpinLock> guard(SpinLock_);
            PushBlock(&guard, result);
        }
        promise.Set(result);
    }

    TSharedRef OnPrefetched()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        return PopBlock(&guard);
    }

    void PushBlock(TGuard<TSpinLock>* guard, const TErrorOr<TSharedRef>& result)
    {
        Y_ASSERT(OutstandingResult_);
        OutstandingResult_.Reset();
        if (!result.IsOK()) {
            Error_ = TError(result);
            return;
        }
        const auto& block = result.Value();
        PrefetchedBlocks_.push(block);
        PrefetchedSize_ += block.Size();
        if (block && PrefetchedSize_ < WindowSize_) {
            Prefetch(guard);
        }
    }

    TSharedRef PopBlock(TGuard<TSpinLock>* guard)
    {
        Y_ASSERT(!PrefetchedBlocks_.empty());
        auto block = PrefetchedBlocks_.front();
        PrefetchedBlocks_.pop();
        PrefetchedSize_ -= block.Size();
        if (!OutstandingResult_ && PrefetchedSize_ < WindowSize_) {
            Prefetch(guard);
        }
        return block;
    }

};

IAsyncZeroCopyInputStreamPtr CreatePrefetchingAdapter(
    IAsyncZeroCopyInputStreamPtr underlyingStream,
    size_t windowSize)
{
    return New<TPrefetchingInputStreamAdapter>(underlyingStream, windowSize);
}

////////////////////////////////////////////////////////////////////////////////

struct TBufferingInputStreamAdapterBufferTag { };

class TBufferingInputStreamAdapter
    : public IAsyncZeroCopyInputStream
{
public:
    TBufferingInputStreamAdapter(
        IAsyncInputStreamPtr underlyingStream,
        size_t windowSize)
        : UnderlyingStream_(underlyingStream)
        , WindowSize_(windowSize)
    {
        YCHECK(UnderlyingStream_);
        YCHECK(WindowSize_ > 0);

        Buffer_ = TSharedMutableRef::Allocate<TBufferingInputStreamAdapterBufferTag>(WindowSize_, false);
    }

    virtual TFuture<TSharedRef> Read() override
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (PrefetchedSize_ == 0) {
            if (EndOfStream_) {
                return MakeFuture<TSharedRef>(TSharedRef());
            }
            if (!Error_.IsOK()) {
                return MakeFuture<TSharedRef>(Error_);
            }
            return Prefetch(&guard).Apply(
                BIND(&TBufferingInputStreamAdapter::OnPrefetched, MakeStrong(this)));
        }
        return MakeFuture<TSharedRef>(CopyPrefetched(&guard));
    }

private:
    const IAsyncInputStreamPtr UnderlyingStream_;
    const size_t WindowSize_;

    TSpinLock SpinLock_;
    TError Error_;
    TSharedMutableRef Prefetched_;
    TSharedMutableRef Buffer_;
    size_t PrefetchedSize_ = 0;
    bool EndOfStream_ = false;
    TFuture<void> OutstandingResult_;

    TFuture<void> Prefetch(TGuard<TSpinLock>* guard)
    {
        if (OutstandingResult_) {
            guard->Release();
            return OutstandingResult_;
        }
        auto promise = NewPromise<void>();
        OutstandingResult_ = promise;
        guard->Release();
        UnderlyingStream_->Read(Buffer_.Slice(0, WindowSize_ - PrefetchedSize_)).Subscribe(BIND(
            &TBufferingInputStreamAdapter::OnRead,
            MakeStrong(this),
            promise));
        return promise;
    }

    void OnRead(TPromise<void> promise, const TErrorOr<size_t>& result)
    {
        {
            TGuard<TSpinLock> guard(SpinLock_);
            AppendPrefetched(&guard, result);
        }
        promise.Set(result);
    }

    TSharedRef OnPrefetched()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        Y_ASSERT(PrefetchedSize_ != 0);
        return CopyPrefetched(&guard);
    }

    void AppendPrefetched(TGuard<TSpinLock>* guard, const TErrorOr<size_t>& result)
    {
        Y_ASSERT(OutstandingResult_);
        OutstandingResult_.Reset();
        if (!result.IsOK()) {
            Error_ = TError(result);
            return;
        } else if (result.Value() == 0) {
            EndOfStream_ = true;
            return;
        }
        size_t bytes = result.Value();
        if (bytes != 0) {
            if (PrefetchedSize_ == 0) {
                Prefetched_ = Buffer_;
                Buffer_ = TSharedMutableRef::Allocate<TBufferingInputStreamAdapterBufferTag>(WindowSize_, false);
            } else {
                ::memcpy(Prefetched_.Begin() + PrefetchedSize_, Buffer_.Begin(), bytes);
            }
            PrefetchedSize_ += bytes;

        }
        // Stop reading on the end of stream or full buffer.
        if (bytes != 0 && PrefetchedSize_ < WindowSize_) {
            Prefetch(guard);
        }
    }

    TSharedRef CopyPrefetched(TGuard<TSpinLock>* guard)
    {
        Y_ASSERT(PrefetchedSize_ != 0);
        auto block = Prefetched_.Slice(0, PrefetchedSize_);
        Prefetched_ = TSharedMutableRef();
        PrefetchedSize_ = 0;
        if (!OutstandingResult_) {
            Prefetch(guard);
        }
        return block;
    }

};

IAsyncZeroCopyInputStreamPtr CreateBufferingAdapter(
    IAsyncInputStreamPtr underlyingStream,
    size_t windowSize)
{
    return New<TBufferingInputStreamAdapter>(underlyingStream, windowSize);
}

////////////////////////////////////////////////////////////////////////////////

class TExpiringInputStreamAdapter
    : public IAsyncZeroCopyInputStream
{
public:
    TExpiringInputStreamAdapter(
        IAsyncZeroCopyInputStreamPtr underlyingStream,
        TDuration timeout)
        : UnderlyingStream_(underlyingStream)
        , Timeout_(timeout)
    {
        YCHECK(UnderlyingStream_);
        YCHECK(Timeout_ > TDuration::Zero());
    }

    virtual TFuture<TSharedRef> Read() override
    {
        TGuard<TSpinLock> guard(SpinLock_);

        if (PendingBlock_) {
            auto block = std::move(PendingBlock_);
            PendingBlock_ = TNullable<TErrorOr<TSharedRef>>();

            return MakeFuture<TSharedRef>(*block);
        }

        auto promise = NewPromise<TSharedRef>();
        Cookie_ = TDelayedExecutor::Submit(
            BIND(&TExpiringInputStreamAdapter::OnTimeout, MakeWeak(this), promise), Timeout_);

        Y_ASSERT(!Promise_);
        Promise_ = promise;

        if (!Fetching_) {
            Fetching_ = true;
            guard.Release();

            UnderlyingStream_->Read().Subscribe(
                BIND(&TExpiringInputStreamAdapter::OnRead, MakeWeak(this)));
        }
        return promise;
    }

private:
    const IAsyncZeroCopyInputStreamPtr UnderlyingStream_;
    const TDuration Timeout_;

    TSpinLock SpinLock_;

    bool Fetching_ = false;
    TNullable<TErrorOr<TSharedRef>> PendingBlock_;
    TPromise<TSharedRef> Promise_;
    TDelayedExecutorCookie Cookie_;

    void OnRead(const TErrorOr<TSharedRef>& value)
    {
        TPromise<TSharedRef> promise;
        TGuard<TSpinLock> guard(SpinLock_);
        Fetching_ = false;
        if (Promise_) {
            swap(Promise_, promise);
            TDelayedExecutor::CancelAndClear(Cookie_);
            guard.Release();
            promise.Set(value);
        } else {
            PendingBlock_ = value;
        }

    }

    void OnTimeout(TPromise<TSharedRef> promise)
    {
        bool timedOut = false;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (promise == Promise_) {
                Promise_ = TPromise<TSharedRef>();
                timedOut = true;
            }
        }

        if (timedOut) {
            promise.Set(TError(NYT::EErrorCode::Timeout, "Operation timed out")
                << TErrorAttribute("timeout", Timeout_));
        }
    }
};

IAsyncZeroCopyInputStreamPtr CreateExpiringAdapter(
    IAsyncZeroCopyInputStreamPtr underlyingStream,
    TDuration timeout)
{
    return New<TExpiringInputStreamAdapter>(underlyingStream, timeout);
}

////////////////////////////////////////////////////////////////////////////////

class TConcurrentInputStreamAdapter
    : public IAsyncZeroCopyInputStream
{
public:
    TConcurrentInputStreamAdapter(
        IAsyncZeroCopyInputStreamPtr underlyingStream)
        : UnderlyingStream_(underlyingStream)
    {
        YCHECK(UnderlyingStream_);
    }

    virtual TFuture<TSharedRef> Read() override
    {
        TGuard<TSpinLock> guard(SpinLock_);

        if (PendingBlock_) {
            auto block = std::move(PendingBlock_);
            PendingBlock_ = TNullable<TErrorOr<TSharedRef>>();

            return MakeFuture<TSharedRef>(*block);
        }

        auto newPromise = NewPromise<TSharedRef>();
        auto oldPromise = newPromise;
        swap(oldPromise, Promise_);

        if (!Fetching_) {
            Fetching_ = true;
            guard.Release();

            UnderlyingStream_->Read().Subscribe(
                BIND(&TConcurrentInputStreamAdapter::OnRead, MakeWeak(this)));
        }
        // Always set the pending promise from previous Read.
        if (oldPromise) {
            guard.Release();
            oldPromise.TrySet(TError(NYT::EErrorCode::Canceled, "Read canceled"));
        }
        return newPromise;
    }

private:
    const IAsyncZeroCopyInputStreamPtr UnderlyingStream_;

    TSpinLock SpinLock_;

    bool Fetching_ = false;
    TNullable<TErrorOr<TSharedRef>> PendingBlock_;
    TPromise<TSharedRef> Promise_;

    void OnRead(const TErrorOr<TSharedRef>& value)
    {
        TPromise<TSharedRef> promise;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            Fetching_ = false;
            Y_ASSERT(Promise_);
            swap(promise, Promise_);
            if (promise.IsSet()) {
                Y_ASSERT(!PendingBlock_);
                PendingBlock_ = value;
                return;
            }
        }
        promise.Set(value);
    }
};

IAsyncZeroCopyInputStreamPtr CreateConcurrentAdapter(
    IAsyncZeroCopyInputStreamPtr underlyingStream)
{
    return New<TConcurrentInputStreamAdapter>(underlyingStream);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
