#include "subprocess.h"
#include "finally.h"
#include "proc.h"

#include <yt/core/logging/log.h>

#include <yt/core/pipes/async_reader.h>

#include <util/system/execpath.h>

#include <array>

namespace NYT {

using namespace NConcurrency;
using namespace NPipes;

////////////////////////////////////////////////////////////////////////////////

const static size_t PipeBlockSize = 64 * 1024;
static NLogging::TLogger Logger("Subprocess");

////////////////////////////////////////////////////////////////////////////////

TSubprocess::TSubprocess(const Stroka& path)
    : Process_(New<TProcess>(path))
{ }

TSubprocess TSubprocess::CreateCurrentProcessSpawner()
{
    return TSubprocess(GetExecPath());
}

void TSubprocess::AddArgument(TStringBuf arg)
{
    Process_->AddArgument(arg);
}

void TSubprocess::AddArguments(std::initializer_list<TStringBuf> args)
{
    Process_->AddArguments(args);
}

TSubprocessResult TSubprocess::Execute()
{
#ifdef _unix_
    auto outputStream = Process_->GetStdOutReader();
    auto errorStream = Process_->GetStdErrReader();
    auto finished = Process_->Spawn();

    auto readIntoBlob = [] (IAsyncInputStreamPtr stream) {
        TBlob output;
        auto buffer = TSharedMutableRef::Allocate(PipeBlockSize, false);
        while (true) {
            auto size = WaitFor(stream->Read(buffer))
                .ValueOrThrow();

            if (size == 0)
                break;

            // ToDo(psushin): eliminate copying.
            output.Append(buffer.Begin(), size);
        }
        return TSharedRef::FromBlob(std::move(output));
    };

    std::vector<TFuture<TSharedRef>> futures = {
        BIND(readIntoBlob, outputStream).AsyncVia(GetCurrentInvoker()).Run(),
        BIND(readIntoBlob, errorStream).AsyncVia(GetCurrentInvoker()).Run(),
    };

    try {
        auto outputsOrError = WaitFor(Combine(futures));
        THROW_ERROR_EXCEPTION_IF_FAILED(
            outputsOrError, 
            "IO error occured during subprocess call");

        const auto& outputs = outputsOrError.Value();
        YCHECK(outputs.size() == 2);

        // This can block indefinitely.
        auto exitCode = WaitFor(finished);
        return TSubprocessResult{outputs[0], outputs[1], exitCode};
    } catch (...) {
        try {
            Process_->Kill(SIGKILL);
        } catch (...) { }
        WaitFor(finished);
        throw;
    }
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
}

void TSubprocess::Kill(int signal)
{
    Process_->Kill(signal);
}

Stroka TSubprocess::GetCommandLine() const
{
    return Process_->GetCommandLine();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
