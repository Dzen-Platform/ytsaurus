#include "new.h"
#include "common.h"
#include "ref_counted_tracker.h"

#include <yt/core/misc/common.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TRefCountedTypeCookie GetRefCountedTypeCookie(
	TRefCountedTypeKey typeKey,
	const TSourceLocation& location)
{
    return TRefCountedTracker::Get()->GetCookie(typeKey, location);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
