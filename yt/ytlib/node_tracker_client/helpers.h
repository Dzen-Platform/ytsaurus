#pragma once

#include "public.h"

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/node_tracker_client/node.pb.h>

#include <yt/core/profiling/profiler.h>

#include <yt/core/yson/public.h>

namespace NYT {
namespace NNodeTrackerClient {

////////////////////////////////////////////////////////////////////////////////

#define ITERATE_NODE_RESOURCES(XX) \
    XX(user_slots,            UserSlots) \
    XX(cpu,                   Cpu) \
    XX(memory,                Memory) \
    XX(network,               Network) \
    XX(replication_slots,     ReplicationSlots) \
    XX(replication_data_size, ReplicationDataSize) \
    XX(removal_slots,         RemovalSlots) \
    XX(repair_slots,          RepairSlots) \
    XX(repair_data_size,      RepairDataSize) \
    XX(seal_slots,            SealSlots)

// NB: Types must be numbered from 0 to N - 1.
DEFINE_ENUM(EResourceType,
    (UserSlots)
    (Cpu)
    (Memory)
    (Network)
    (ReplicationSlots)
    (ReplicationDataSize)
    (RemovalSlots)
    (RepairSlots)
    (RepairDataSize)
    (SealSlots)
);

Stroka FormatResourceUsage(const NProto::TNodeResources& usage, const NProto::TNodeResources& limits);
Stroka FormatResources(const NProto::TNodeResources& resources);

const NProto::TNodeResources& ZeroNodeResources();
const NProto::TNodeResources& InfiniteNodeResources();

NObjectClient::TObjectId ObjectIdFromNodeId(TNodeId nodeId, NObjectClient::TCellTag);
TNodeId NodeIdFromObjectId(const NObjectClient::TObjectId& objectId);

namespace NProto {

TNodeResources  operator +  (const TNodeResources& lhs, const TNodeResources& rhs);
TNodeResources& operator += (TNodeResources& lhs, const TNodeResources& rhs);

TNodeResources  operator -  (const TNodeResources& lhs, const TNodeResources& rhs);
TNodeResources& operator -= (TNodeResources& lhs, const TNodeResources& rhs);

TNodeResources  operator *  (const TNodeResources& lhs, i64 rhs);
TNodeResources  operator *  (const TNodeResources& lhs, double rhs);
TNodeResources& operator *= (TNodeResources& lhs, i64 rhs);
TNodeResources& operator *= (TNodeResources& lhs, double rhs);

TNodeResources  operator -  (const TNodeResources& resources);

bool operator == (const TNodeResources& a, const TNodeResources& b);
bool operator != (const TNodeResources& a, const TNodeResources& b);

NProto::TNodeResources MakeNonnegative(const NProto::TNodeResources& resources);
bool Dominates(const NProto::TNodeResources& lhs, const TNodeResources& rhs);

TNodeResources Max(const TNodeResources& a, const TNodeResources& b);
TNodeResources Min(const TNodeResources& a, const TNodeResources& b);

void Serialize(
    const NProto::TNodeResources& resources,
    NYson::IYsonConsumer* consumer);

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT
