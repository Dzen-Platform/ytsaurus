#ifndef SORTED_DYNAMIC_STORE_INL_H_
#error "Direct inclusion of this file is not allowed, include sorted_dynamic_store.h"
// For the sake of sane code completion.
#include "sorted_dynamic_store.h"
#endif

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

TTimestamp TSortedDynamicStore::TimestampFromRevision(ui32 revision) const
{
    return RevisionToTimestamp_[revision];
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
