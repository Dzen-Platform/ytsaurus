#include "public.h"

#include <yt/ytlib/driver/driver.h>

#include <yt/core/concurrency/async_stream.h>

#include <util/system/mutex.h>

#include <contrib/libs/pycxx/Extensions.hxx>

namespace NYT {
namespace NPython {

///////////////////////////////////////////////////////////////////////////////

class TBufferedStream
    : public NConcurrency::IAsyncOutputStream
{
public:
    explicit TBufferedStream(size_t capacity);

    // Called from python.
    PyObject* Read(size_t size = 0);

    // Called from python.
    bool Empty() const;

    // Called from YT.
    void Finish();

    // Called from YT.
    virtual TFuture<void> Write(const TSharedRef& data) override;

    virtual TFuture<void> Close() override;

private:
    TSharedMutableRef Data_;
    char* Begin_;
    size_t Size_ = 0;

    size_t Capacity_ = 0;

    // Number of bytes that waited by read command.
    size_t SizeToRead_ = 0;

    // Marks that writes to the stream are finished.
    bool Finished_ = false;

    // Marks that inner buffer is full (size >= capacity / 2) and writes should wait.
    bool Full_ = false;

    // Marks that buffer contains enough bytes to be read by waiting read command.
    std::atomic<bool> AllowRead_;

    // Marks that stream ready to receive more bytes.
    TPromise<void> AllowWrite_;

    TMutex Mutex_;
    TMutex ReadMutex_;

    void Reallocate(size_t len);
    void Move(char* dest);
    PyObject* ExtractChunk(size_t size);
};

DEFINE_REFCOUNTED_TYPE(TBufferedStream)

///////////////////////////////////////////////////////////////////////////////

class TBufferedStreamWrap
    : public Py::PythonClass<TBufferedStreamWrap>
{
public:
    TBufferedStreamWrap(Py::PythonClassInstance* self, Py::Tuple& args, Py::Dict& kwargs);

    Py::Object Read(Py::Tuple& args, Py::Dict& kwargs);
    PYCXX_KEYWORDS_METHOD_DECL(TBufferedStreamWrap, Read);

    Py::Object Empty(Py::Tuple& args, Py::Dict& kwargs);
    PYCXX_KEYWORDS_METHOD_DECL(TBufferedStreamWrap, Empty);

    TBufferedStreamPtr GetStream();

    virtual ~TBufferedStreamWrap();

    static void InitType();

private:
    TBufferedStreamPtr Stream_;
};

///////////////////////////////////////////////////////////////////////////////

} // namespace NPython
} // namespace NYT
