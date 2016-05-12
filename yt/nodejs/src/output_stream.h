#pragma once

#include "stream_base.h"

#include <yt/core/actions/future.h>

#include <util/system/mutex.h>

namespace NYT {
namespace NNodeJS {

////////////////////////////////////////////////////////////////////////////////

//! This class adheres to TOutputStream interface as a C++ object and
//! simultaneously provides 'readable stream' interface stubs as a JS object
//! thus effectively acting as a bridge from C++ to JS.
class TOutputStreamWrap
    : public TNodeJSStreamBase
    , public TOutputStream
{
protected:
    TOutputStreamWrap(ui64 watermark);
    ~TOutputStreamWrap() throw();

public:
    static v8::Persistent<v8::FunctionTemplate> ConstructorTemplate;
    static void Initialize(v8::Handle<v8::Object> target);
    static bool HasInstance(v8::Handle<v8::Value> value);

    // Synchronous JS API.
    static v8::Handle<v8::Value> New(const v8::Arguments& args);

    static v8::Handle<v8::Value> Pull(const v8::Arguments& args);
    v8::Handle<v8::Value> DoPull();

    static v8::Handle<v8::Value> Destroy(const v8::Arguments& args);
    void DoDestroy();

    static v8::Handle<v8::Value> IsFlowing(const v8::Arguments& args);
    v8::Handle<v8::Value> DoIsFlowing();

    static v8::Handle<v8::Value> IsFinished(const v8::Arguments& args);
    v8::Handle<v8::Value> DoIsFinished();

    // Diagnostics.
    const ui64 GetBytesEnqueued() const;
    const ui64 GetBytesDequeued() const;

    void MarkAsFinishing();

protected:
    // C++ API.
    virtual void DoWrite(const void* buffer, size_t length) override;
    virtual void DoWriteV(const TPart* parts, size_t count) override;
    virtual void DoFinish() override;

private:
    bool CanFlow() const;
    void RunFlow(bool withinV8);
    static int AsyncOnFlowing(eio_req* request);

    void ProtectedUpdateAndNotifyWriter(std::function<void()> mutator);
    void PushToQueue(std::unique_ptr<char[]> blob, size_t length);

private:
    const ui64 Watermark_;

    // Protects everything below.
    TMutex Mutex_;

    bool IsFlowing_ = false;
    bool IsFinishing_ = false;
    bool IsFinished_ = false;
    bool IsDestroyed_ = false;

    ui64 BytesInFlight_ = 0;
    ui64 BytesEnqueued_ = 0;
    ui64 BytesDequeued_ = 0;

    TPromise<void> WritePromise_;
    std::deque<TOutputPart> Queue_;

private:
    TOutputStreamWrap(const TOutputStreamWrap&) = delete;
    TOutputStreamWrap& operator=(const TOutputStreamWrap&) = delete;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeJS
} // namespace NYT
