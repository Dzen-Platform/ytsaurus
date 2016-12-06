#include "shutdown.h"

#include <yt/ytlib/shutdown.h>

#include <contrib/libs/pycxx/Objects.hxx>

namespace NYT {
namespace NPython {

///////////////////////////////////////////////////////////////////////////////

static TCallback<void()> AdditionalShutdownCallback;

void Shutdown()
{
    AdditionalShutdownCallback.Run();
    NYT::Shutdown();
}

void RegisterShutdown(TCallback<void()> additionalCallback)
{
    static bool registered = false;

    if (!registered) {
        AdditionalShutdownCallback = additionalCallback;
        registered = true;
        Py_AtExit(Shutdown);
    }
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NPython
} // namespace NYT

