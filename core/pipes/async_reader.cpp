#include "async_reader.h"
#include "private.h"
#include "io_dispatcher.h"
#include "io_dispatcher_impl.h"
#include "pipe.h"

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/proc.h>

#include <yt/contrib/libev/ev++.h>

#include <errno.h>

namespace NYT {
namespace NPipes {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = PipesLogger;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EReaderState,
    (Active)
    (EndOfStream)
    (Failed)
    (Aborted)
);

class TAsyncReaderImpl
    : public TRefCounted
{
public:
    explicit TAsyncReaderImpl(int fd)
        : FD_(fd)
    {
        BIND([=, this_ = MakeStrong(this)] () {
            InitWatcher();
        })
        .Via(TIODispatcher::Get()->GetInvoker())
        .Run();
    }

    explicit TAsyncReaderImpl(const Stroka& str)
    {
        BIND([=, this_ = MakeStrong(this)] () {
            if (!InitFD(str)) {
                return;
            }
            InitWatcher();
        })
        .Via(TIODispatcher::Get()->GetInvoker())
        .Run();
    }

    ~TAsyncReaderImpl()
    {
        YCHECK(State_ != EReaderState::Active || AbortRequested_);
    }

    int GetHandle() const
    {
        return FD_;
    }

    TFuture<size_t> Read(const TSharedMutableRef& buffer)
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YCHECK(buffer.Size() > 0);

        auto promise = NewPromise<size_t>();

        TIODispatcher::Get()->GetInvoker()->Invoke(BIND([=, this_ = MakeStrong(this)] () {
            YCHECK(ReadResultPromise_.IsSet());
            ReadResultPromise_ = promise;

            switch (State_) {
                case EReaderState::Aborted:
                    ReadResultPromise_.Set(TError(EErrorCode::Aborted, "Reader aborted")
                        << TErrorAttribute("fd", FD_));
                    break;

                case EReaderState::EndOfStream:
                    ReadResultPromise_.Set(0);
                    break;

                case EReaderState::Failed:
                    ReadResultPromise_.Set(Error_);
                    break;

                case EReaderState::Active:
                    Buffer_ = buffer;
                    Position_ = 0;
                    if (!FDWatcher_.is_active()) {
                        FDWatcher_.start();
                    }
                    break;

                default:
                    YUNREACHABLE();
            };
        }));

        return promise.ToFuture();
    }

    TFuture<void> Abort()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        AbortRequested_ = true;
        return BIND([=, this_ = MakeStrong(this)] () {
                if (State_ == EReaderState::Active) { 
                    State_ = EReaderState::Aborted;
                    FDWatcher_.stop();
                    ReadResultPromise_.TrySet(TError(EErrorCode::Aborted, "Reader aborted")
                        << TErrorAttribute("fd", FD_));
                    Close();
                }
            })
            .AsyncVia(TIODispatcher::Get()->GetInvoker())
            .Run();
    }

private:
    int FD_ = -1;

    //! \note Thread-unsafe. Must be accessed from ev-thread only.
    ev::io FDWatcher_;

    TPromise<size_t> ReadResultPromise_ = MakePromise<size_t>(0);

    std::atomic<bool> AbortRequested_ = { false };
    EReaderState State_ = EReaderState::Active;
    TError Error_;

    TSharedMutableRef Buffer_;
    int Position_ = 0;

    DECLARE_THREAD_AFFINITY_SLOT(EventLoop);


    void OnRead(ev::io&, int eventType)
    {
        VERIFY_THREAD_AFFINITY(EventLoop);
        YCHECK((eventType & ev::READ) == ev::READ);
        YCHECK(State_ == EReaderState::Active);

        while (!ReadResultPromise_.IsSet()) {
            DoRead();
            YCHECK(Position_ != 0 || ReadResultPromise_.IsSet());
        }
    }

    void DoRead()
    {
#ifdef _unix_
        YCHECK(Position_ < Buffer_.Size());
        YCHECK(!ReadResultPromise_.IsSet());

        ssize_t size = HandleEintr(::read, FD_, Buffer_.Begin() + Position_, Buffer_.Size() - Position_);

        if (size == -1) {
            if (errno == EAGAIN) {
                if (Position_ != 0) {
                    FDWatcher_.stop();
                    ReadResultPromise_.Set(Position_);
                }
                return;
            }

            YCHECK(errno != EBADF);

            Error_ = TError("Reader failed")
                << TErrorAttribute("fd", FD_)
                << TError::FromSystem();
            LOG_ERROR(Error_);
            Close();

            State_ = EReaderState::Failed;
            FDWatcher_.stop();
            if (Position_ != 0) {
                ReadResultPromise_.Set(Position_);
            } else {
                ReadResultPromise_.Set(Error_);
            }
            return;
        }

        Position_ += size;

        if (size == 0) {
            State_ = EReaderState::EndOfStream;
            FDWatcher_.stop();
            Close();
            ReadResultPromise_.Set(Position_);
        } else if (Position_ == Buffer_.Size()) {
            FDWatcher_.stop();
            ReadResultPromise_.Set(Position_);
        }
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
    }

    bool InitFD(const Stroka& str)
    {
        FD_ = HandleEintr(::open, str.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (FD_ == -1) {
            State_ = EReaderState::Failed;
            Error_ = TError("Open failed")
                << TErrorAttribute("path", str)
                << TError::FromSystem();
            LOG_ERROR(Error_);
            return false;
        }
        return true;
    }

    void InitWatcher()
    {
        FDWatcher_.set(FD_, ev::READ);
        FDWatcher_.set(TIODispatcher::Get()->GetEventLoop());
        FDWatcher_.set<TAsyncReaderImpl, &TAsyncReaderImpl::OnRead>(this);
    }

    void Close()
    {
        YCHECK(TryClose(FD_, false));
        FD_ = TPipe::InvalidFD;
    }
};

DEFINE_REFCOUNTED_TYPE(TAsyncReaderImpl);

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

TAsyncReader::TAsyncReader(int fd)
    : Impl_(New<NDetail::TAsyncReaderImpl>(fd))
{ }

TAsyncReader::TAsyncReader(TNamedPipePtr ptr)
    : Impl_(New<NDetail::TAsyncReaderImpl>(ptr->GetPath()))
    , NamedPipeHolder_(ptr)
{ }

TAsyncReader::~TAsyncReader()
{
    // Abort does not fail.
    Impl_->Abort();
}

int TAsyncReader::GetHandle() const
{
    return Impl_->GetHandle();
}

TFuture<size_t> TAsyncReader::Read(const TSharedMutableRef& buffer)
{
    return Impl_->Read(buffer);
}

TFuture<void> TAsyncReader::Abort()
{
    return Impl_->Abort();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
