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
    explicit TSubprocess(const TString& path);

    static TSubprocess CreateCurrentProcessSpawner();

    void AddArgument(TStringBuf arg);
    void AddArguments(std::initializer_list<TStringBuf> args);

    TSubprocessResult Execute();
    void Kill(int signal);

    TString GetCommandLine() const;

private:
    TProcessPtr Process_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
