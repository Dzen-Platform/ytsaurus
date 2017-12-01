#include "connection.h"
#include "private.h"

#include <yt/core/concurrency/poller.h>

#include <yt/core/misc/proc.h>
#include <yt/core/net/socket.h>
#include <yt/core/misc/finally.h>

#include <errno.h>

namespace NYT {
namespace NNet {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

struct TIOResult
{
    bool Retry;
    i64 ByteCount;

    TIOResult(bool retry, i64 byteCount)
        : Retry(retry)
        , ByteCount(byteCount)
    { }
};

struct IIOOperation
{
    virtual ~IIOOperation() = default;

    virtual TErrorOr<TIOResult> PerformIO(int fd) = 0;

    virtual void Abort(const TError& error) = 0;

    virtual void SetResult() = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TReadOperation
    : public IIOOperation
{
public:
    explicit TReadOperation(const TSharedMutableRef& buffer)
        : Buffer_(buffer)
    { }

    virtual TErrorOr<TIOResult> PerformIO(int fd) override
    {
        size_t bytesRead = 0;
        while (true) {
            ssize_t size = HandleEintr(::read, fd, Buffer_.Begin() + Position_, Buffer_.Size() - Position_);

            if (size == -1) {
                if (errno == EAGAIN) {
                    return TIOResult(Position_ == 0, bytesRead);
                }

                return TError("Read failed")
                    << TError::FromSystem();
            }

            Position_ += size;

            if (Position_ == Buffer_.Size() || size == 0) {
                return TIOResult(false, bytesRead);
            }
        }
    }

    virtual void Abort(const TError& error) override
    {
        ResultPromise_.Set(error);
    }

    virtual void SetResult() override
    {
        ResultPromise_.Set(Position_);
    }

    TFuture<size_t> ToFuture() const
    {
        return ResultPromise_.ToFuture();
    }

private:
    TSharedMutableRef Buffer_;
    size_t Position_ = 0;

    TPromise<size_t> ResultPromise_ = NewPromise<size_t>();
};

////////////////////////////////////////////////////////////////////////////////

class TWriteOperation
    : public IIOOperation
{
public:
    explicit TWriteOperation(const TSharedRef& buffer)
        : Buffer_(buffer)
    { }

    virtual TErrorOr<TIOResult> PerformIO(int fd) override
    {
        size_t bytesWritten = 0;
        while (true) {
            YCHECK(Position_ < Buffer_.Size());
            ssize_t size = HandleEintr(::write, fd, Buffer_.Begin() + Position_, Buffer_.Size() - Position_);

            if (size == -1) {
                if (errno == EAGAIN) {
                    return TIOResult(true, bytesWritten);
                }

                return TError("Write failed")
                    << TError::FromSystem();
            }

            YCHECK(size > 0);
            bytesWritten += size;
            Position_ += size;

            if (Position_ == Buffer_.Size()) {
                return TIOResult(false, bytesWritten);
            }
        }
    }

    virtual void Abort(const TError& error) override
    {
        ResultPromise_.Set(error);
    }

    virtual void SetResult() override
    {
        ResultPromise_.Set();
    }

    TFuture<void> ToFuture() const
    {
        return ResultPromise_.ToFuture();
    }

private:
    TSharedRef Buffer_;
    size_t Position_ = 0;

    TPromise<void> ResultPromise_ = NewPromise<void>();
};

////////////////////////////////////////////////////////////////////////////////

class TWriteVOperation
    : public IIOOperation
{
public:
    explicit TWriteVOperation(const TSharedRefArray& buffers)
        : Buffers_(buffers)
    { }

    virtual TErrorOr<TIOResult> PerformIO(int fd) override
    {
        size_t bytesWritten = 0;
        while (true) {
            constexpr int MaxEntries = 128;
            iovec ioVectors[MaxEntries];

            ioVectors[0].iov_base = reinterpret_cast<void*>(const_cast<char*>(Buffers_[Index_].Begin() + Position_));
            ioVectors[0].iov_len = Buffers_[Index_].Size() - Position_;

            size_t ioVectorsCount = 1;
            for (; ioVectorsCount < MaxEntries && ioVectorsCount + Index_ < Buffers_.Size(); ++ioVectorsCount) {
                const auto& ref = Buffers_[Index_ + ioVectorsCount];
            
                ioVectors[ioVectorsCount].iov_base = reinterpret_cast<void*>(const_cast<char*>(ref.Begin()));
                ioVectors[ioVectorsCount].iov_len = ref.Size();
            }
        
            ssize_t size = HandleEintr(::writev, fd, ioVectors, ioVectorsCount);

            if (size == -1) {
                if (errno == EAGAIN) {
                    return TIOResult(true, bytesWritten);
                }

                return TError("Write failed")
                    << TError::FromSystem();
            }

            YCHECK(size > 0);
            bytesWritten += size;
            Position_ += size;

            while (Index_ != Buffers_.Size() && Position_ >= Buffers_[Index_].Size()) {
                Position_ -= Buffers_[Index_].Size();
                Index_++;
            }

            if (Index_ == Buffers_.Size()) {
                return TIOResult(false, bytesWritten);
            }
        }
    }

    virtual void Abort(const TError& error) override
    {
        ResultPromise_.Set(error);
    }

    virtual void SetResult() override
    {
        ResultPromise_.Set();
    }

    TFuture<void> ToFuture() const
    {
        return ResultPromise_.ToFuture();
    }

private:
    TSharedRefArray Buffers_;
    size_t Index_ = 0;
    size_t Position_ = 0;

    TPromise<void> ResultPromise_ = NewPromise<void>();
};

////////////////////////////////////////////////////////////////////////////////

class TShutdownOperation
    : public IIOOperation
{
public:
    explicit TShutdownOperation(bool shutdownRead)
        : ShutdownRead_(shutdownRead)
    { }

    virtual TErrorOr<TIOResult> PerformIO(int fd) override
    {
        int res = HandleEintr(::shutdown, fd, ShutdownRead_ ? SHUT_RD : SHUT_WR);
        if (res == -1) {
            return TError("Shutdown failed")
                << TError::FromSystem();
        }
        return TIOResult(false, 0);
    }

    virtual void Abort(const TError& error) override
    {
        ResultPromise_.Set(error);
    }

    virtual void SetResult() override
    {
        ResultPromise_.Set();
    }

    TFuture<void> ToFuture() const
    {
        return ResultPromise_.ToFuture();
    }

private:
    const bool ShutdownRead_;
    TPromise<void> ResultPromise_ = NewPromise<void>();
};

////////////////////////////////////////////////////////////////////////////////

class TFDConnectionImpl
    : public IPollable
{
public:
    TFDConnectionImpl(
        int fd,
        const TNetworkAddress& localAddress,
        const TNetworkAddress& remoteAddress,
        const IPollerPtr& poller)
        : Name_(Format("FDConnection{%v<->%v}", localAddress, remoteAddress))
        , LocalAddress_(localAddress)
        , RemoteAddress_(remoteAddress)
        , FD_(fd)
        , Poller_(poller)
    {
        ReadDirection_.DoIO = BIND(&TFDConnectionImpl::DoIO, MakeStrong(this), &ReadDirection_, false);
        ReadDirection_.PollFlag = EPollControl::Read;
        WriteDirection_.DoIO = BIND(&TFDConnectionImpl::DoIO, MakeStrong(this), &WriteDirection_, false);
        WriteDirection_.PollFlag = EPollControl::Write;
        Poller_->Register(this);
    }

    virtual const TString& GetLoggingId() const override
    {
        return Name_;
    }

    virtual void OnEvent(EPollControl control) override
    {
        if (Any(control & EPollControl::Write)) {
            DoIO(&WriteDirection_, true);
        }

        if (Any(control & EPollControl::Read)) {
            DoIO(&ReadDirection_, true);
        }
    }

    virtual void OnShutdown() override
    {
        // Poller gurantees that OnShutdown is never executed concurrently with OnEvent()
        // but it may execute concurrently with callback what was posted directly to
        // the poller invoker. In that case we postpone closing the descriptor until
        // callback finished executing.
        {
            auto guard = Guard(Lock_);

            // IO callback is responsible for closing file descriptor and setting future.
            if (ReadDirection_.Starting || WriteDirection_.Starting) {
                AbortDelayed_ = true;
                return;
            }

            YCHECK(!Error_.IsOK());
        }

        // At this point Error_, ReadDirection_, WriteDirection_ can't change.
        // We access them directly, without spinlock.
        if (ReadDirection_.Operation) {
            ReadDirection_.Operation->Abort(Error_);
            ReadDirection_.Operation.reset();
        }
        if (WriteDirection_.Operation) {
            WriteDirection_.Operation->Abort(Error_);
            WriteDirection_.Operation.reset();
        }
        FinishShutdown();
    }
    
    TFuture<size_t> Read(const TSharedMutableRef& data)
    {
        auto read = std::make_unique<TReadOperation>(data);
        auto future = read->ToFuture();
        StartIO(&ReadDirection_, std::move(read));
        return future;
    }

    TFuture<void> Write(const TSharedRef& data)
    {
        auto write = std::make_unique<TWriteOperation>(data);
        auto future = write->ToFuture();
        StartIO(&WriteDirection_, std::move(write));
        return future;
    }

    TFuture<void> WriteV(const TSharedRefArray& data)
    {
        auto writeV = std::make_unique<TWriteVOperation>(data);
        auto future = writeV->ToFuture();
        StartIO(&WriteDirection_, std::move(writeV));
        return future;
    }

    TFuture<void> Close()
    {
        auto error = TError("Connection closed")
            << TErrorAttribute("connection", Name_);
        return AbortIO(error);
    }

    TFuture<void> Abort(const TError& error)
    {
        return AbortIO(error);
    }

    TFuture<void> CloseRead()
    {
        auto shutdownRead = std::make_unique<TShutdownOperation>(true);
        auto future = shutdownRead->ToFuture();
        StartIO(&ReadDirection_, std::move(shutdownRead));
        return future;
    }

    TFuture<void> CloseWrite()
    {
        auto shutdownWrite = std::make_unique<TShutdownOperation>(false);
        auto future = shutdownWrite->ToFuture();
        StartIO(&WriteDirection_, std::move(shutdownWrite));
        return future;
    }

    const TNetworkAddress& LocalAddress() const
    {
        return LocalAddress_;
    }

    const TNetworkAddress& RemoteAddress() const
    {
        return RemoteAddress_;
    }

private:
    const TString Name_;
    const TNetworkAddress LocalAddress_;
    const TNetworkAddress RemoteAddress_;
    int FD_ = -1;

    struct TIODirection
    {
        std::unique_ptr<IIOOperation> Operation;
        bool Starting = false;
        std::atomic<i64> BytesTransferred = {0};

        TClosure DoIO;
        EPollControl PollFlag;
    };

    TSpinLock Lock_;
    TIODirection ReadDirection_;
    TIODirection WriteDirection_;
    bool AbortDelayed_ = false;

    TError Error_;
    TPromise<void> ShutdownPromise_ = NewPromise<void>();

    EPollControl Control_ = EPollControl::None;
    IPollerPtr Poller_;

    void StartIO(TIODirection* direction, std::unique_ptr<IIOOperation> operation)
    {
        TError error;

        {
            auto guard = Guard(Lock_);
            if (Error_.IsOK()) {
                YCHECK(!direction->Operation);
                direction->Operation = std::move(operation);

                YCHECK(!direction->Starting);
                direction->Starting = true;
            } else {
                error = Error_;
            }
        }

        if (!error.IsOK()) {
            operation->Abort(error);
            return;
        }

        Poller_->GetInvoker()->Invoke(direction->DoIO);
    }

    void DoIO(TIODirection* direction, bool filterSpuriousEvent)
    {
        {
            auto guard = Guard(Lock_);
            // We use Poller in a way that generates spurious
            // notifications. Do nothing if we are not interested in
            // this event.
            if (filterSpuriousEvent && None(Control_ & direction->PollFlag)) {
                return;
            }

            if (filterSpuriousEvent) {
                Control_ ^= direction->PollFlag;
            }
        }

        YCHECK(direction->Operation);
        auto result = direction->Operation->PerformIO(FD_);
        if (result.IsOK()) {
            direction->BytesTransferred += result.Value().ByteCount;
        } else {
            result << TErrorAttribute("connection", Name_);
        }
        
        bool needClose = false;
        std::unique_ptr<IIOOperation> operation;
        {
            auto guard = Guard(Lock_);
            if (!result.IsOK()) {
                // IO finished with error.
                operation = std::move(direction->Operation);
                if (Error_.IsOK()) {
                    Error_ = result;
                    UnregisterFromPoller();
                }
            } else if (!Error_.IsOK()) {
                // IO was aborted.
                operation = std::move(direction->Operation);
                // Avoid aborting completed IO.
                if (result.Value().Retry) {
                    result = Error_;
                }
            } else if (result.Value().Retry) {
                // IO not completed.
                Control_ |= direction->PollFlag;
            } else {
                // IO finished successfully.
                operation = std::move(direction->Operation);
            }

            direction->Starting = false;
            if (AbortDelayed_ && !WriteDirection_.Starting && !ReadDirection_.Starting) {
                // Object is in the middle of shutdown. We are responsible for closing FD_.
                needClose = true;
            }

            MaybeRearm();
        }

        if (!result.IsOK()) {
            operation->Abort(result);
        } else if (!result.Value().Retry) {
            operation->SetResult();
        }

        if (needClose) {
            FinishShutdown();
        }
    }

    TFuture<void> AbortIO(const TError& error)
    {
        auto guard = Guard(Lock_);
        if (Error_.IsOK()) {
            Error_ = error;
            UnregisterFromPoller();
        }
        return ShutdownPromise_.ToFuture();
    }
    
    void UnregisterFromPoller()
    {
        Poller_->Unarm(FD_);
        Poller_->Unregister(this);
    }

    void MaybeRearm()
    {
        if (Control_ != EPollControl::None) {
            Poller_->Arm(FD_, this, Control_);
        }
    }
    
    void FinishShutdown()
    {
        YCHECK(TryClose(FD_, false));
        FD_ = -1;

        ReadDirection_.DoIO.Reset();
        WriteDirection_.DoIO.Reset();
        ShutdownPromise_.Set();
    }
};

DECLARE_REFCOUNTED_CLASS(TFDConnectionImpl)
DEFINE_REFCOUNTED_TYPE(TFDConnectionImpl)

////////////////////////////////////////////////////////////////////////////////

// The sole purpose of this class is to call Abort on Impl in dtor.
class TFDConnection
    : public IConnection
{
public:
    TFDConnection(
        int fd,
        const TNetworkAddress& localAddress,
        const TNetworkAddress& remoteAddress,
        const IPollerPtr& poller)
        : Impl_(New<TFDConnectionImpl>(fd, localAddress, remoteAddress, poller))
    { }

    ~TFDConnection()
    {
        Impl_->Abort(TError("Connection is abandoned"));
    }

    virtual const TNetworkAddress& LocalAddress() const override
    {
        return Impl_->LocalAddress();
    }

    virtual const TNetworkAddress& RemoteAddress() const override
    {
        return Impl_->RemoteAddress();
    }
    
    virtual TFuture<size_t> Read(const TSharedMutableRef& data) override
    {
        return Impl_->Read(data);
    }

    virtual TFuture<void> Write(const TSharedRef& data) override
    {
        return Impl_->Write(data);
    }

    virtual TFuture<void> WriteV(const TSharedRefArray& data) override
    {
        return Impl_->WriteV(data);
    }

    virtual TFuture<void> Close() override
    {
        return Impl_->Close();
    }

    virtual TFuture<void> CloseRead() override
    {
        return Impl_->CloseRead();
    }

    virtual TFuture<void> CloseWrite() override
    {
        return Impl_->CloseWrite();
    }
    
private:
    const TFDConnectionImplPtr Impl_;
};

////////////////////////////////////////////////////////////////////////////////

std::pair<IConnectionPtr, IConnectionPtr> CreateConnectionPair(const IPollerPtr& poller)
{
    int flags = SOCK_STREAM;
#ifdef _linux_
    flags |= SOCK_NONBLOCK | SOCK_CLOEXEC;
#endif

    int fds[2];
    if (HandleEintr(::socketpair, AF_LOCAL, flags, 0, fds) == -1) {
        THROW_ERROR_EXCEPTION("Failed to create socket pair")
            << TError::FromSystem();
    }

    try {
        auto address0 = GetSocketName(fds[0]);
        auto address1 = GetSocketName(fds[1]);

        auto first = New<TFDConnection>(fds[0], address0, address1, poller);
        auto second = New<TFDConnection>(fds[1], address1, address0, poller);
        return std::make_pair(std::move(first), std::move(second));
    } catch (...) {
        YCHECK(TryClose(fds[0], false));
        YCHECK(TryClose(fds[1], false));
        throw;
    }
}

IConnectionPtr CreateConnectionFromFD(
    int fd,
    const TNetworkAddress& localAddress,
    const TNetworkAddress& remoteAddress,
    const NConcurrency::IPollerPtr& poller)
{
    return New<TFDConnection>(fd, localAddress, remoteAddress, poller);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNet
} // namespace NYT
