#include "assert.h"

#include "crash_handler.h"
#include "raw_formatter.h"
#include "safe_assert.h"
#include "proc.h"

#include <yt/core/misc/core_dumper.h>
#include <yt/core/misc/string.h>

#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/logging/log_manager.h>

#ifdef _win_
    #include <io.h>
#else
    #include <unistd.h>
#endif

#include <errno.h>

namespace NYT {
namespace NDetail {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

void AssertTrapImpl(
    const char* trapType,
    const char* expr,
    const char* file,
    int line)
{
    TRawFormatter<1024> formatter;
    formatter.AppendString(trapType);
    formatter.AppendString("(");
    formatter.AppendString(expr);
    formatter.AppendString(") at ");
    formatter.AppendString(file);
    formatter.AppendString(":");
    formatter.AppendNumber(line);
    formatter.AppendString("\n");

    if (SafeAssertionsModeEnabled()) {
        auto semaphore = GetSafeAssertionsCoreSemaphore();
        TNullable<Stroka> corePath;
        if (auto semaphoreGuard = TAsyncSemaphoreGuard::TryAcquire(semaphore)) {
            try {
                auto coreDump = GetSafeAssertionsCoreDumper()->WriteCoreDump({
                    "Reason: SafeAssertion"
                });
                corePath = coreDump.Path;
                // A tricky way to return slot only after core is written.
                coreDump.WrittenEvent.Subscribe(BIND([_ = std::move(semaphoreGuard)] (TError /* error */) { }));
            } catch (std::exception&) {
                // Do nothing.
            }
        }
        TStringBuilder stackTrace;
        DumpStackTrace([&stackTrace] (const char* buffer, int length) {
            stackTrace.AppendString(TStringBuf(buffer, length));
        });
        Stroka expression(formatter.GetData(), formatter.GetBytesWritten());
        throw TAssertionFailedException(std::move(expression), stackTrace.Flush(), std::move(corePath));
    } else {
        HandleEintr(::write, 2, formatter.GetData(), formatter.GetBytesWritten());

        NLogging::TLogManager::Get()->Shutdown();

        BUILTIN_TRAP();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
