#pragma once

#include "public.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/node_tracker_server/public.h>

#include <yt/yt/core/concurrency/public.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/misc/optional.h>

namespace NYT::NCellServer {

////////////////////////////////////////////////////////////////////////////////

class TCellTracker
    : public TRefCounted
{
public:
    explicit TCellTracker(NCellMaster::TBootstrap* bootstrap);

    ~TCellTracker();

    void Start();
    void Stop();

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TCellTracker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellServer
