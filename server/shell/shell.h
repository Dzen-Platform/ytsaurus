#pragma once

#include "public.h"

#include <yt/core/actions/future.h>

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/ref.h>

namespace NYT {
namespace NShell {

////////////////////////////////////////////////////////////////////////////////

struct TShellOptions
{
    Stroka ExePath = "/bin/bash";
    TNullable<int> Uid;
    Stroka Term = "xterm";
    int Height = 24;
    int Width = 80;
    Stroka WorkingDir = "/";
    TNullable<Stroka> CGroupBasePath;
    std::vector<Stroka> Environment;
    TNullable<Stroka> Bashrc;
    TNullable<Stroka> MessageOfTheDay;
    TDuration InactivityTimeout;
    TNullable<Stroka> Command;
};

////////////////////////////////////////////////////////////////////////////////

//! Represents a shell running inside job sandbox.
struct IShell
    : public virtual TRefCounted
{
    virtual const TShellId& GetId() = 0;
    virtual void ResizeWindow(int height, int width) = 0;
    //! Inserts keys into input sequence at specified offset.
    //! Returns consumed offset of the input sequence.
    //! This function is NOT thread-safe.
    virtual ui64 SendKeys(const TSharedRef& keys, ui64 inputOffset) = 0;
    virtual TFuture<TSharedRef> Poll() = 0;
    //! Tries to clean up, best effort guarantees.
    virtual void Terminate(const TError& error) = 0;
    //! Asynchronously wait for inactivity timeout and terminate.
    virtual TFuture<void> Shutdown(const TError& error) = 0;
};

DEFINE_REFCOUNTED_TYPE(IShell)

////////////////////////////////////////////////////////////////////////////////

IShellPtr CreateShell(std::unique_ptr<TShellOptions> options);

////////////////////////////////////////////////////////////////////////////////

} // namespace NShell
} // namespace NYT
