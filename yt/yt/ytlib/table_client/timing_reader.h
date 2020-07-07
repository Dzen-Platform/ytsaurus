#pragma once

#include "timing_statistics.h"

#include <yt/client/chunk_client/ready_event_reader_base.h>

#include <yt/core/profiling/timing.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct ITimingReader
    : public virtual TRefCounted
{
    virtual TTimingStatistics GetTimingStatistics() const = 0;
};

DEFINE_REFCOUNTED_TYPE(ITimingReader)

////////////////////////////////////////////////////////////////////////////////

class TTimingReaderBase
    : public virtual ITimingReader
    , public NChunkClient::TReadyEventReaderBase
{
protected:
    virtual TTimingStatistics GetTimingStatistics() const override;

    NProfiling::TTimerGuard<NProfiling::TWallTimer> AcquireReadGuard();

private:
    NProfiling::TWallTimer TotalTimer_;
    NProfiling::TWallTimer ReadTimer_ = NProfiling::TWallTimer(false /* start */);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
