#pragma once

#include "public.h"

#include <yt/ytlib/hive/public.h>

#include <yt/ytlib/api/native/public.h>

#include <yt/core/concurrency/public.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TTableReplicator
    : public TRefCounted
{
public:
    TTableReplicator(
        TTabletManagerConfigPtr config,
        TTablet* tablet,
        TTableReplicaInfo* replicaInfo,
        NApi::NNative::IConnectionPtr localConnection,
        TTabletSlotPtr slot,
        ITabletSnapshotStorePtr tabletSnapshotStore,
        IHintManagerPtr hintManager,
        IInvokerPtr workerInvoker,
        NConcurrency::IThroughputThrottlerPtr nodeInThrottler,
        NConcurrency::IThroughputThrottlerPtr nodeOutThrottler);
    ~TTableReplicator();

    void Enable();
    void Disable();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TTableReplicator)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
