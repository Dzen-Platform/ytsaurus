#pragma once

#include "public.h"

#include <yt/core/misc/process.h>
#include <yt/core/misc/ref.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TSubprocessResult
{
    TSharedRef Output;
    TSharedRef Error;
    TError Status;
};

////////////////////////////////////////////////////////////////////////////////

class TSubprocess
{
public:
    explicit TSubprocess(const Stroka& path);

    static TSubprocess CreateCurrentProcessSpawner();

    void AddArgument(TStringBuf arg);
    void AddArguments(std::initializer_list<TStringBuf> args);

    TSubprocessResult Execute();
    void Kill(int signal);

    Stroka GetCommandLine() const;

private:
    const TProcessBasePtr Process_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
