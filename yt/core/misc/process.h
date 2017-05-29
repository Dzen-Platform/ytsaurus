#pragma once

#include "error.h"

#include <yt/core/actions/future.h>

#include <yt/core/concurrency/public.h>

#include <yt/core/pipes/pipe.h>

#include <yt/core/containers/public.h>

#include <yt/contrib/portoapi/libporto.hpp>

#include <atomic>
#include <vector>
#include <array>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TProcessBase
    : public TRefCounted
{
public:
    explicit TProcessBase(const Stroka& path);

    void AddArgument(TStringBuf arg);
    void AddEnvVar(TStringBuf var);

    void AddArguments(std::initializer_list<TStringBuf> args);
    void AddArguments(const std::vector<Stroka>& args);

    void SetWorkingDirectory(const Stroka& path);

    virtual NPipes::TAsyncWriterPtr GetStdInWriter() = 0;
    virtual NPipes::TAsyncReaderPtr GetStdOutReader() = 0;
    virtual NPipes::TAsyncReaderPtr GetStdErrReader() = 0;

    TFuture<void> Spawn();
    virtual void Kill(int signal) = 0;

    Stroka GetPath() const;
    int GetProcessId() const;
    bool IsStarted() const;
    bool IsFinished() const;

    Stroka GetCommandLine() const;

protected:
    const Stroka Path_;

    int ProcessId_;
    std::atomic<bool> Started_ = {false};
    std::atomic<bool> Finished_ = {false};
    int MaxSpawnActionFD_ = - 1;
    NPipes::TPipe Pipe_;
    std::vector<Stroka> StringHolders_;
    std::vector<const char*> Args_;
    std::vector<const char*> Env_;
    Stroka ResolvedPath_;
    Stroka WorkingDirectory_;
    TPromise<void> FinishedPromise_ = NewPromise<void>();

    virtual void DoSpawn() = 0;
    const char* Capture(const TStringBuf& arg);

private:
    void SpawnChild();
    void ValidateSpawnResult();
    void Child();
    void AsyncPeriodicTryWait();
};

DEFINE_REFCOUNTED_TYPE(TProcessBase)


////////////////////////////////////////////////////////////////////////////////

// Read this
// http://ewontfix.com/7/
// before making any changes.
class TSimpleProcess
    : public TProcessBase
{
public:
    explicit TSimpleProcess(
        const Stroka& path,
        bool copyEnv = true,
        TDuration pollPeriod = TDuration::MilliSeconds(100));
    virtual void Kill(int signal) override;
    virtual NPipes::TAsyncWriterPtr GetStdInWriter() override;
    virtual NPipes::TAsyncReaderPtr GetStdOutReader() override;
    virtual NPipes::TAsyncReaderPtr GetStdErrReader() override;

private:
    const TDuration PollPeriod_;

    NPipes::TPipeFactory PipeFactory_;
    std::array<NPipes::TPipe, 3> StdPipes_;

    NConcurrency::TPeriodicExecutorPtr AsyncWaitExecutor_;
    struct TSpawnAction
    {
        std::function<bool()> Callback;
        Stroka ErrorMessage;
    };

    std::vector<TSpawnAction> SpawnActions_;

    void AddDup2FileAction(int oldFD, int newFD);
    virtual void DoSpawn() override;
    void SpawnChild();
    void ValidateSpawnResult();
    void AsyncPeriodicTryWait();
    void Child();
};

////////////////////////////////////////////////////////////////////////////////

class TPortoProcess
    : public TProcessBase
{
public:
    TPortoProcess(
        const Stroka& path,
        NContainers::IInstancePtr containerInstance,
        bool copyEnv = true,
        TDuration pollPeriod = TDuration::MilliSeconds(100));
    virtual void Kill(int signal) override;
    virtual NPipes::TAsyncWriterPtr GetStdInWriter() override;
    virtual NPipes::TAsyncReaderPtr GetStdOutReader() override;
    virtual NPipes::TAsyncReaderPtr GetStdErrReader() override;

private:
    NContainers::IInstancePtr ContainerInstance_;
    std::vector<NPipes::TNamedPipePtr> NamedPipes_;
    virtual void DoSpawn() override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
