#pragma once

#include "public.h"

#include <yt/core/misc/nullable.h>

namespace NYT {
namespace NJobProberClient {

////////////////////////////////////////////////////////////////////////////////

TNullable<int> FindSignalIdBySignalName(const Stroka& signalName);
void ValidateSignalName(const Stroka& signalName);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProberClient
} // namespace NYT
