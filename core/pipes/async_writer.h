#pragma once

#include "public.h"

#include <yt/core/actions/future.h>

#include <yt/core/concurrency/async_stream.h>

namespace NYT {
namespace NPipes {

////////////////////////////////////////////////////////////////////////////////

//! Implements IAsyncOutputStream interface on top of a file descriptor.
class TAsyncWriter
    : public NConcurrency::IAsyncOutputStream
{
public:
    //! Takes ownership of #fd.
    explicit TAsyncWriter(int fd);

    explicit TAsyncWriter(TNamedPipePtr ptr);

    virtual ~TAsyncWriter();

    int GetHandle() const;

    virtual TFuture<void> Write(const TSharedRef& buffer) override;

    TFuture<void> Close();

    //! Thread-safe, can be called multiple times.
    TFuture<void> Abort();

private:
    NDetail::TAsyncWriterImplPtr Impl_;
    TNamedPipePtr NamedPipeHolder_;
};

DEFINE_REFCOUNTED_TYPE(TAsyncWriter);

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
