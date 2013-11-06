#include "buffered_stream.h"

#include "common.h"

#include <core/actions/future.h>

namespace NYT {
namespace NPython {

///////////////////////////////////////////////////////////////////////////////

TBufferedStream::TBufferedStream(i64 bufferSize)
    : Size_(0)
    , AllowedSize_(bufferSize / 2)
    , Data_(TSharedRef::Allocate(bufferSize))
    , Begin_(Data_.Begin())
    , End_(Data_.Begin())
    , State_(EState::Normal)
    , AllowWrite_(NewPromise<TError>())
    , AllowRead_(NewPromise())
{ }

TSharedRef TBufferedStream::Read(i64 size)
{
    YCHECK(State_ != EState::WaitingData);

    if (Size_ >= size) {
        return ExtractChunk(size);
    }

    bool wait = false;
    {
        TGuard<TMutex> guard(Mutex_);
        if (State_ == EState::Full) {
            AllowedSize_ = std::max(AllowedSize_, size);
            AllowWrite_.Set(TError());
        }

        if (State_ != EState::Finished)
        {
            wait = true;
            State_ = EState::WaitingData;
            AllowRead_ = NewPromise();
        }
    }

    if (wait) {
        AllowRead_.Get();
    }

    return ExtractChunk(size);
}

bool TBufferedStream::Empty() const
{
    return Size_ == 0;
}


void TBufferedStream::Finish(NDriver::TDriverResponse)
{
    TGuard<TMutex> guard(Mutex_);

    YASSERT(State_ != EState::Finished);

    if (State_ == EState::WaitingData) {
        AllowRead_.Set();
    }
    if (State_ == EState::Full) {
        AllowWrite_.Set(TError());
    }

    State_ = EState::Finished;
}

bool TBufferedStream::Write(const void* buf, size_t len)
{
    YCHECK(State_ != EState::Full);

    {
        TGuard<TMutex> guard(Mutex_);

        if (Data_.End() - End_ < len) {
            if (Size_ + len > Data_.Size()) {
                Reallocate(std::max(Size_ + len, Data_.Size() * 2));
            } else if (End_ - Begin_ <= Begin_ - Data_.Begin()) {
                Move(Data_.Begin());
            } else {
                Reallocate(Data_.Size());
            }
        }

        auto buf_ = reinterpret_cast<const char*>(buf);
        std::copy(buf_, buf_ + len, End_);
        End_ = End_ + len;
        Size_ += len;
    }

    if (Size_ >= AllowedSize_) {
        TGuard<TMutex> guard(Mutex_);

        if (State_ == EState::WaitingData) {
            AllowRead_.Set();
        }

        AllowWrite_ = NewPromise<TError>();
        State_ = EState::Full;

        return false;
    } else {
        return true;
    }
}

TAsyncError TBufferedStream::GetReadyEvent()
{
    return AllowWrite_;
}

void TBufferedStream::Reallocate(size_t len)
{
    auto newData = TSharedRef::Allocate(len);
    Move(newData.Begin());
    std::swap(Data_, newData);
}

void TBufferedStream::Move(char* dest)
{
    std::copy(Begin_, End_, dest);
    End_ = dest + (End_ - Begin_);
    Begin_ = dest;
}

TSharedRef TBufferedStream::ExtractChunk(i64 size)
{
    TGuard<TMutex> guard(Mutex_);

    size = std::min(size, End_ - Begin_);

    TSharedRef result = Data_.Slice(TRef(Begin_, size));
    Begin_ += size;

    Size_ -= size;
    if (Size_ < AllowedSize_ && State_ == EState::Full) {
        AllowWrite_.Set(TError());
        State_ = EState::Normal;
    }

    return result;
}

///////////////////////////////////////////////////////////////////////////////

TBufferedStreamWrap::TBufferedStreamWrap(Py::PythonClassInstance *self, Py::Tuple& args, Py::Dict& kwargs)
    : Py::PythonClass<TBufferedStreamWrap>::PythonClass(self, args, kwargs)
    , Stream_(New<TBufferedStream>(Py::Int(ExtractArgument(args, kwargs, "size")).asLongLong()))
{
    if (args.length() > 0 || kwargs.length() > 0) {
        throw Py::RuntimeError("Incorrect arguments for read command");
    }
}

Py::Object TBufferedStreamWrap::Read(Py::Tuple& args, Py::Dict& kwargs)
{
    auto size = Py::Int(ExtractArgument(args, kwargs, "size"));
    if (args.length() > 0 || kwargs.length() > 0) {
        throw Py::RuntimeError("Incorrect arguments for read function");
    }

    auto result = Stream_->Read(size.asLongLong());
    return Py::String(result.Begin(), result.Size());
}

Py::Object TBufferedStreamWrap::Empty(Py::Tuple& args, Py::Dict& kwargs)
{
    if (args.length() > 0 || kwargs.length() > 0) {
        throw Py::RuntimeError("Incorrect arguments for empty function");
    }
    return Py::Boolean(Stream_->Empty());
}

TBufferedStreamPtr TBufferedStreamWrap::GetStream()
{
    return Stream_;
}

TBufferedStreamWrap::~TBufferedStreamWrap()
{ }

void TBufferedStreamWrap::InitType()
{
    behaviors().name("BufferedStream");
    behaviors().doc("Buffered stream to perform read and download asynchronously");
    behaviors().supportGetattro();
    behaviors().supportSetattro();

    PYCXX_ADD_KEYWORDS_METHOD(read, Read, "Synchronously read data from stream");
    PYCXX_ADD_KEYWORDS_METHOD(empty, Empty, "Check wether the stream is empty");

    behaviors().readyType();
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NPython
} // namespace NYT
