#include "helpers.h"

#include <yt/ytlib/chunk_client/medium_directory.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/misc/arithmetic_formula.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/yson_serializable.h>

#include <limits>

namespace NYT::NNodeTrackerClient {

using namespace NYson;
using namespace NYTree;
using namespace NProfiling;
using namespace NObjectClient;
using namespace NNodeTrackerClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TString FormatResources(
    const TNodeResources& usage,
    const TNodeResources& limits)
{
    return Format(
        "UserSlots: %v/%v, Cpu: %v/%v, Gpu: %v/%v, UserMemory: %v/%v, SystemMemory: %v/%v, Network: %v/%v, "
        "ReplicationSlots: %v/%v, ReplicationDataSize: %v/%v, "
        "RemovalSlots: %v/%v, "
        "RepairSlots: %v/%v, RepairDataSize: %v/%v, "
        "SealSlots: %v/%v",
        // User slots
        usage.user_slots(),
        limits.user_slots(),
        // Cpu
        usage.cpu(),
        limits.cpu(),
        // Gpu,
        usage.gpu(),
        limits.gpu(),
        // User memory (in MB)
        usage.user_memory() / 1_MB,
        limits.user_memory() / 1_MB,
        // System memory (in MB)
        usage.system_memory() / 1_MB,
        limits.system_memory() / 1_MB,
        // Network
        usage.network(),
        limits.network(),
        // Replication slots
        usage.replication_slots(),
        limits.replication_slots(),
        // Replication data size
        usage.replication_data_size(),
        limits.replication_data_size(),
        // Removal slots
        usage.removal_slots(),
        limits.removal_slots(),
        // Repair slots
        usage.repair_slots(),
        limits.repair_slots(),
        // Repair data size
        usage.repair_data_size(),
        limits.repair_data_size(),
        // Seal slots
        usage.seal_slots(),
        limits.seal_slots());
}

TString FormatResourceUsage(
    const TNodeResources& usage,
    const TNodeResources& limits)
{
    return Format("{%v}", FormatResources(usage, limits));
}

void FormatValue(TStringBuilderBase* builder, const NProto::TDiskResources& diskResources, TStringBuf spec)
{
    MakeFormattableView(diskResources.disk_location_resources(), [] (TStringBuilderBase* builder, const NProto::TDiskLocationResources& locationResources) {
        builder->AppendFormat("{usage: %v, limit: %v, medium_index: %v}",
            locationResources.usage(),
            locationResources.limit(),
            locationResources.medium_index());
    });
}

TString ToString(const NProto::TDiskResources& diskResources)
{
    return ToStringViaBuilder(diskResources);
}

TString ToString(const NProto::TDiskResources& diskResources, const NChunkClient::TMediumDirectoryPtr& mediumDirectory)
{
    return Format(
        "%v",
        MakeFormattableView(diskResources.disk_location_resources(), [&mediumDirectory] (TStringBuilderBase* builder, const NProto::TDiskLocationResources& locationResources) {
            int mediumIndex = locationResources.medium_index();
            auto* mediumDescriptor = mediumDirectory->FindByIndex(mediumIndex);
            TStringBuf mediumName = mediumDescriptor
                ? mediumDescriptor->Name
                : TStringBuf("unknown");
            builder->AppendFormat("{usage: %v, limit: %v, medium_index: %v, medium_name: %v}",
                locationResources.usage(),
                locationResources.limit(),
                mediumIndex,
                mediumName);
        })
    );
}

TString FormatResourceUsage(
    const TNodeResources& usage,
    const TNodeResources& limits,
    const TDiskResources& diskResources)
{
    return Format("{%v, DiskResources: %v}", FormatResources(usage, limits), ToString(diskResources));
}

TString FormatResources(const TNodeResources& resources)
{
    return Format(
        "{"
        "UserSlots: %v, Cpu: %v, Gpu: %v, UserMemory: %vMB, SystemMemory: %vMB, Network: %v, "
        "ReplicationSlots: %v, ReplicationDataSize: %vMB, "
        "RemovalSlots: %v, "
        "RepairSlots: %v, RepairDataSize: %vMB, "
        "SealSlots: %v"
        "}",
        resources.user_slots(),
        resources.cpu(),
        resources.gpu(),
        resources.user_memory() / 1_MB,
        resources.system_memory() / 1_MB,
        resources.network(),
        resources.replication_slots(),
        resources.replication_data_size() / 1_MB,
        resources.removal_slots(),
        resources.repair_slots(),
        resources.repair_data_size() / 1_MB,
        resources.seal_slots());
}

void ProfileResources(TLegacyProfiler& profiler, const TNodeResources& resources)
{
    #define XX(name, Name) profiler.Enqueue("/" #name, resources.name(), EMetricType::Gauge);
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
}

void ProfileResources(ISensorWriter* writer, const NProto::TNodeResources& resources)
{
    #define XX(name, Name) writer->AddGauge("/" #name, resources.name());
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
}

TNodeResources GetZeroNodeResources()
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(0);
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

const TNodeResources& ZeroNodeResources()
{
    static auto value = GetZeroNodeResources();
    return value;
}

TNodeResources GetInfiniteResources()
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(std::numeric_limits<decltype(result.name())>::max() / 4);
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

const TNodeResources& InfiniteNodeResources()
{
    static auto result = GetInfiniteResources();
    return result;
}

TObjectId ObjectIdFromNodeId(TNodeId nodeId, TCellTag cellTag)
{
    return MakeId(EObjectType::ClusterNode, cellTag, nodeId, 0);
}

TNodeId NodeIdFromObjectId(TObjectId objectId)
{
    return CounterFromId(objectId);
}

void ValidateNodeTags(const std::vector<TString>& tags)
{
    for (const auto& tag : tags) {
        try {
            ValidateBooleanFormulaVariable(tag);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Invalid node tag %Qv", tag)
                << ex;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

TNodeResources operator + (const TNodeResources& lhs, const TNodeResources& rhs)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(lhs.name() + rhs.name());
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

TNodeResources& operator += (TNodeResources& lhs, const TNodeResources& rhs)
{
    #define XX(name, Name) lhs.set_##name(lhs.name() + rhs.name());
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return lhs;
}

TNodeResources operator - (const TNodeResources& lhs, const TNodeResources& rhs)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(lhs.name() - rhs.name());
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

TNodeResources& operator -= (TNodeResources& lhs, const TNodeResources& rhs)
{
    #define XX(name, Name) lhs.set_##name(lhs.name() - rhs.name());
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return lhs;
}

TNodeResources operator * (const TNodeResources& lhs, i64 rhs)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(lhs.name() * rhs);
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

TNodeResources operator * (const TNodeResources& lhs, double rhs)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(static_cast<decltype(lhs.name())>(lhs.name() * rhs + 0.5));
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

TNodeResources& operator *= (TNodeResources& lhs, i64 rhs)
{
    #define XX(name, Name) lhs.set_##name(lhs.name() * rhs);
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return lhs;
}

TNodeResources& operator *= (TNodeResources& lhs, double rhs)
{
    #define XX(name, Name) lhs.set_##name(static_cast<decltype(lhs.name())>(lhs.name() * rhs + 0.5));
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return lhs;
}

TNodeResources  operator - (const TNodeResources& resources)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(-resources.name());
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

bool operator == (const TNodeResources& lhs, const TNodeResources& rhs)
{
    return
        #define XX(name, Name) lhs.name() == rhs.name() &&
        ITERATE_NODE_RESOURCES(XX)
        #undef XX
        true;
}

bool operator != (const TNodeResources& lhs, const TNodeResources& rhs)
{
    return !(lhs == rhs);
}

TNodeResources MakeNonnegative(const TNodeResources& resources)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(std::max(resources.name(), static_cast<decltype(resources.name())>(0)));
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

bool Dominates(const TNodeResources& lhs, const TNodeResources& rhs)
{
    return
        #define XX(name, Name) lhs.name() >= rhs.name() &&
        ITERATE_NODE_RESOURCES(XX)
        #undef XX
        true;
}

TNodeResources Max(const TNodeResources& a, const TNodeResources& b)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(std::max(a.name(), b.name()));
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

TNodeResources Min(const TNodeResources& a, const TNodeResources& b)
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(std::min(a.name(), b.name()));
    ITERATE_NODE_RESOURCES(XX)
    #undef XX
    return result;
}

void Serialize(const TNodeResources& resources, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            #define XX(name, Name) .Item(#name).Value(resources.name())
            ITERATE_NODE_RESOURCES(XX)
            #undef XX
        .EndMap();
}

class TSerializableNodeResourceLimitsOverrides
    : public NYTree::TYsonSerializableLite
{
public:
    #define XX(name, Name) std::optional<decltype(TNodeResourceLimitsOverrides::default_instance().name())> Name;
    ITERATE_NODE_RESOURCE_LIMITS_OVERRIDES(XX)
    #undef XX

    TSerializableNodeResourceLimitsOverrides()
    {
        #define XX(name, Name) \
            RegisterParameter(#name, Name) \
                .GreaterThanOrEqual(0) \
                .Optional();
        ITERATE_NODE_RESOURCE_LIMITS_OVERRIDES(XX)
        #undef XX
    }
};

void Serialize(const TNodeResourceLimitsOverrides& overrides, IYsonConsumer* consumer)
{
    TSerializableNodeResourceLimitsOverrides serializableOverrides;
    #define XX(name, Name) \
        if (overrides.has_##name()) { \
            serializableOverrides.Name = overrides.name(); \
        }
    ITERATE_NODE_RESOURCE_LIMITS_OVERRIDES(XX)
    #undef XX
    Serialize(serializableOverrides, consumer);
}

void Deserialize(TNodeResourceLimitsOverrides& overrides, INodePtr node)
{
    TSerializableNodeResourceLimitsOverrides serializableOverrides;
    Deserialize(serializableOverrides, node);
    #define XX(name, Name) \
        if (serializableOverrides.Name) { \
            overrides.set_##name(*serializableOverrides.Name); \
        } else { \
            overrides.clear_##name(); \
        }
    ITERATE_NODE_RESOURCE_LIMITS_OVERRIDES(XX)
    #undef XX
}

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerClient

