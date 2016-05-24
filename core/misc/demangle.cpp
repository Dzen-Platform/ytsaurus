#include "demangle.h"

#include <yt/core/misc/common.h>

#if defined(__GNUC__)
#include <util/memory/tempbuf.h>
#include <exception>
#include <cxxabi.h>
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

Stroka DemangleCxxName(const char* mangledName)
{
#if defined(__GNUC__)
    TTempBuf buffer;

    size_t returnedLength = buffer.Size();
    int returnedStatus = 0;

    abi::__cxa_demangle(mangledName, buffer.Data(), &returnedLength, &returnedStatus);
    return Stroka(returnedStatus == 0 ? buffer.Data() : mangledName);
#else
    return Stroka(mangledName);
#endif
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
