#pragma once

#include "public.h"
#include "row_base.h"

#include <yt/ytlib/chunk_client/schema.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger TableClientLogger;
extern const NProfiling::TProfiler TableClientProfiler;

template <class TPredicate>
int LowerBound(int lowerIndex, int upperIndex, const TPredicate& less)
{
    while (upperIndex - lowerIndex > 0) {
        auto middle = (upperIndex + lowerIndex) / 2;
        if (less(middle)) {
            lowerIndex = middle + 1;
        } else {
            upperIndex = middle;
        }
    }
    return lowerIndex;
}

void ValidateKeyColumns(const TKeyColumns& keyColumns, const TKeyColumns& chunkKeyColumns);

TColumnFilter CreateColumnFilter(const NChunkClient::TChannel& protoChannel, TNameTablePtr nameTable);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
