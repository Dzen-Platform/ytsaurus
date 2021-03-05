#pragma once

#include <yt/yt/core/misc/public.h>

namespace NYT::NDriver {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IDriver)

DECLARE_REFCOUNTED_CLASS(TDriverConfig)

struct TCommandDescriptor;
struct TDriverRequest;
struct TEtag;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDriver
