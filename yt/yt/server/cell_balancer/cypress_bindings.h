#pragma once

#include "private.h"

#include <yt/yt/client/tablet_client/public.h>

#include <yt/yt/core/ytree/yson_serializable.h>
#include <yt/yt/core/ytree/yson_struct.h>

namespace NYT::NCellBalancer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(TBundleInfo)
DECLARE_REFCOUNTED_STRUCT(THulkInstanceResources)
DECLARE_REFCOUNTED_STRUCT(TInstanceResources)
DECLARE_REFCOUNTED_STRUCT(TResourceQuota)
DECLARE_REFCOUNTED_STRUCT(TResourceLimits)
DECLARE_REFCOUNTED_STRUCT(TBundleConfig)
DECLARE_REFCOUNTED_STRUCT(TBundleSystemOptions)
DECLARE_REFCOUNTED_STRUCT(TCpuLimits)
DECLARE_REFCOUNTED_STRUCT(TMemoryLimits)
DECLARE_REFCOUNTED_STRUCT(TBundleControllerState)
DECLARE_REFCOUNTED_STRUCT(TZoneInfo)
DECLARE_REFCOUNTED_STRUCT(TAllocationRequestSpec)
DECLARE_REFCOUNTED_STRUCT(TAllocationRequestStatus)
DECLARE_REFCOUNTED_STRUCT(TAllocationRequest)
DECLARE_REFCOUNTED_STRUCT(TDeallocationRequestSpec)
DECLARE_REFCOUNTED_STRUCT(TDeallocationRequestStatus)
DECLARE_REFCOUNTED_STRUCT(TDeallocationRequest)
DECLARE_REFCOUNTED_STRUCT(TDeallocationRequestState)
DECLARE_REFCOUNTED_STRUCT(TInstanceAnnotations)
DECLARE_REFCOUNTED_STRUCT(TTabletNodeInfo)
DECLARE_REFCOUNTED_STRUCT(TTabletCellStatus)
DECLARE_REFCOUNTED_STRUCT(TTabletCellInfo)
DECLARE_REFCOUNTED_STRUCT(TTabletCellPeer)
DECLARE_REFCOUNTED_STRUCT(TTabletSlot)
DECLARE_REFCOUNTED_STRUCT(TBundleDynamicConfig)
DECLARE_REFCOUNTED_STRUCT(TRpcProxyAlive)
DECLARE_REFCOUNTED_STRUCT(TMaintenanceRequest)
DECLARE_REFCOUNTED_STRUCT(TRpcProxyInfo)
DECLARE_REFCOUNTED_STRUCT(TAccountResources)
DECLARE_REFCOUNTED_STRUCT(TSystemAccount)

template <typename TEntryInfo>
using TIndexedEntries = THashMap<TString, TIntrusivePtr<TEntryInfo>>;

constexpr int YTRoleTypeTabNode = 1;
constexpr int YTRoleTypeRpcProxy = 3;

inline static const TString InstanceStateOnline = "online";
inline static const TString InstanceStateOffline = "offline";

inline static const TString TabletSlotStateEmpty = "none";

inline static const TString PeerStateLeading = "leading";

inline static const TString DeallocationStrategyHulkRequest = "hulk_deallocation_request";
inline static const TString DeallocationStrategyReturnToBB = "return_to_bundle_balancer";

////////////////////////////////////////////////////////////////////////////////

template <typename TDerived>
class TYsonStructAttributes
    : public NYTree::TYsonStruct
{
public:
    static std::vector<TString> GetAttributes()
    {
        // Making sure attributes are registered.
        // YSON struct registration takes place in constructor.
        static auto holder = New<TDerived>();

        return holder->Attributes_;
    }

    template <typename TRegistrar, typename TValue>
    static auto& RegisterAttribute(TRegistrar registrar, const TString& attribute, TValue(TDerived::*field))
    {
        Attributes_.push_back(attribute);
        return registrar.Parameter(attribute, field);
    }

private:
    inline static std::vector<TString> Attributes_;
};

////////////////////////////////////////////////////////////////////////////////

struct TCpuLimits
    : public NYTree::TYsonStruct
{
    int WriteThreadPoolSize;
    int LookupThreadPoolSize;
    int QueryThreadPoolSize;

    REGISTER_YSON_STRUCT(TCpuLimits);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TCpuLimits)

////////////////////////////////////////////////////////////////////////////////

struct TMemoryLimits
    : public NYTree::TYsonStruct
{
    std::optional<i64> TabletStatic;
    std::optional<i64> TabletDynamic;
    std::optional<i64> BlockCache;
    std::optional<i64> VersionedChunkMeta;
    std::optional<i64> LookupRowCache;

    REGISTER_YSON_STRUCT(TMemoryLimits);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TMemoryLimits)

////////////////////////////////////////////////////////////////////////////////

struct TInstanceResources
    : public NYTree::TYsonStruct
{
    int Vcpu;
    i64 Memory;
    TString Type;

    TInstanceResources& operator=(const THulkInstanceResources& resources);

    bool operator==(const TInstanceResources& resources) const;

    void Clear();

    REGISTER_YSON_STRUCT(TInstanceResources);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TInstanceResources)

////////////////////////////////////////////////////////////////////////////////

struct TResourceQuota
    : public NYTree::TYsonStruct
{
    double Cpu;
    i64 Memory;

    int Vcpu() const;

    REGISTER_YSON_STRUCT(TResourceQuota);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TResourceQuota)

////////////////////////////////////////////////////////////////////////////////

struct TResourceLimits
    : public NYTree::TYsonStruct
{
    i64 TabletStaticMemory;

    REGISTER_YSON_STRUCT(TResourceLimits);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TResourceLimits)

////////////////////////////////////////////////////////////////////////////////

struct TBundleConfig
    : public NYTree::TYsonStruct
{
    int TabletNodeCount;
    int RpcProxyCount;
    TInstanceResourcesPtr TabletNodeResourceGuarantee;
    TInstanceResourcesPtr RpcProxyResourceGuarantee;
    TCpuLimitsPtr CpuLimits;
    TMemoryLimitsPtr MemoryLimits;

    REGISTER_YSON_STRUCT(TBundleConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBundleConfig)

////////////////////////////////////////////////////////////////////////////////

struct TTabletCellStatus
    : public NYTree::TYsonStruct
{
    TString Health;
    bool Decommissioned;

    REGISTER_YSON_STRUCT(TTabletCellStatus);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TTabletCellStatus)

////////////////////////////////////////////////////////////////////////////////

struct TTabletCellPeer
    : public NYTree::TYsonStruct
{
    TString Address;
    TString State;

    REGISTER_YSON_STRUCT(TTabletCellPeer);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TTabletCellPeer)

////////////////////////////////////////////////////////////////////////////////

struct TTabletCellInfo
    : public TYsonStructAttributes<TTabletCellInfo>
{
    TString TabletCellBundle;
    TString TabletCellLifeStage;
    int TabletCount;
    TTabletCellStatusPtr Status;
    std::vector<TTabletCellPeerPtr> Peers;

    REGISTER_YSON_STRUCT(TTabletCellInfo);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TTabletCellInfo)

////////////////////////////////////////////////////////////////////////////////

struct TBundleInfo
    : public TYsonStructAttributes<TBundleInfo>
{
    NTabletClient::ETabletCellHealth Health;
    TString Zone;
    TString NodeTagFilter;
    std::optional<TString> ShortName;

    bool EnableBundleController;
    bool EnableTabletCellManagement;
    bool EnableNodeTagFilterManagement;
    bool EnableTabletNodeDynamicConfig;
    bool EnableRpcProxyManagement;
    bool EnableSystemAccountManagement;
    bool EnableResourceLimitsManagement;

    TBundleConfigPtr TargetConfig;
    TBundleConfigPtr ActualConfig;
    std::vector<TString> TabletCellIds;

    TBundleSystemOptionsPtr Options;
    TResourceQuotaPtr ResourceQuota;
    TResourceLimitsPtr ResourceLimits;

    REGISTER_YSON_STRUCT(TBundleInfo);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBundleInfo)

////////////////////////////////////////////////////////////////////////////////

struct TZoneInfo
    : public TYsonStructAttributes<TZoneInfo>
{
    TString YPCluster;
    TString TabletNodeNannyService;
    TString RpcProxyNannyService;

    int MaxTabletNodeCount;
    int MaxRpcProxyCount;

    TBundleConfigPtr SpareTargetConfig;
    double DisruptedThresholdFactor;

    REGISTER_YSON_STRUCT(TZoneInfo);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TZoneInfo)

////////////////////////////////////////////////////////////////////////////////

struct THulkInstanceResources
    : public NYTree::TYsonStruct
{
    int Vcpu;
    i64 MemoryMb;

    THulkInstanceResources& operator=(const TInstanceResources& resources);

    REGISTER_YSON_STRUCT(THulkInstanceResources);
    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(THulkInstanceResources)

////////////////////////////////////////////////////////////////////////////////

struct TAllocationRequestSpec
    : public NYTree::TYsonStruct
{
    TString YPCluster;
    TString NannyService;
    THulkInstanceResourcesPtr ResourceRequest;
    TString PodIdTemplate;
    int InstanceRole;

    REGISTER_YSON_STRUCT(TAllocationRequestSpec);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TAllocationRequestSpec)

////////////////////////////////////////////////////////////////////////////////

struct TAllocationRequestStatus
    : public NYTree::TYsonStruct
{
    TString State;
    TString NodeId;
    TString PodId;

    REGISTER_YSON_STRUCT(TAllocationRequestStatus);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TAllocationRequestStatus)

////////////////////////////////////////////////////////////////////////////////

struct TAllocationRequest
    : public NYTree::TYsonStruct
{
    TAllocationRequestSpecPtr Spec;
    TAllocationRequestStatusPtr Status;

    REGISTER_YSON_STRUCT(TAllocationRequest);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TAllocationRequest)

////////////////////////////////////////////////////////////////////////////////

struct TDeallocationRequestSpec
    : public NYTree::TYsonStruct
{
    TString YPCluster;
    TString PodId;
    int InstanceRole;

    REGISTER_YSON_STRUCT(TDeallocationRequestSpec);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TDeallocationRequestSpec)

////////////////////////////////////////////////////////////////////////////////

struct TDeallocationRequestStatus
    : public NYTree::TYsonStruct
{
    TString State;

    REGISTER_YSON_STRUCT(TDeallocationRequestStatus);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TDeallocationRequestStatus)

////////////////////////////////////////////////////////////////////////////////

struct TDeallocationRequest
    : public NYTree::TYsonStruct
{
    TDeallocationRequestSpecPtr Spec;
    TDeallocationRequestStatusPtr Status;

    REGISTER_YSON_STRUCT(TDeallocationRequest);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TDeallocationRequest)

////////////////////////////////////////////////////////////////////////////////

struct TAllocationRequestState
    : public NYTree::TYsonStruct
{
    TInstant CreationTime;
    TString PodIdTemplate;

    REGISTER_YSON_STRUCT(TAllocationRequestState);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TAllocationRequestState)

////////////////////////////////////////////////////////////////////////////////

struct TDeallocationRequestState
    : public NYTree::TYsonStruct
{
    TInstant CreationTime;
    TString InstanceName;
    TString Strategy;

    bool HulkRequestCreated;

    REGISTER_YSON_STRUCT(TDeallocationRequestState);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TDeallocationRequestState)

////////////////////////////////////////////////////////////////////////////////

struct TRemovingTabletCellInfo
    : public NYTree::TYsonStruct
{
    TInstant RemovedTime;

    REGISTER_YSON_STRUCT(TRemovingTabletCellInfo);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TRemovingTabletCellInfo)

////////////////////////////////////////////////////////////////////////////////

struct TBundleControllerState
    : public TYsonStructAttributes<TBundleControllerState>
{
    TIndexedEntries<TAllocationRequestState> NodeAllocations;
    TIndexedEntries<TDeallocationRequestState> NodeDeallocations;
    TIndexedEntries<TRemovingTabletCellInfo> RemovingCells;

    TIndexedEntries<TAllocationRequestState> ProxyAllocations;
    TIndexedEntries<TDeallocationRequestState> ProxyDeallocations;

    REGISTER_YSON_STRUCT(TBundleControllerState);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBundleControllerState)

////////////////////////////////////////////////////////////////////////////////

struct TInstanceAnnotations
    : public NYTree::TYsonStruct
{
    TString YPCluster;
    TString NannyService;
    TString AllocatedForBundle;
    bool Allocated;
    TInstanceResourcesPtr Resource;

    std::optional<TInstant> DeallocatedAt;
    TString DeallocationStrategy;

    REGISTER_YSON_STRUCT(TInstanceAnnotations);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TInstanceAnnotations)

////////////////////////////////////////////////////////////////////////////////

struct TTabletSlot
    : public NYTree::TYsonStruct
{
    TString TabletCellBundle;
    TString CellId;
    int PeerId;
    TString State;

    REGISTER_YSON_STRUCT(TTabletSlot);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TTabletSlot)

////////////////////////////////////////////////////////////////////////////////

struct TMaintenanceRequest
    : public NYTree::TYsonStruct
{
    REGISTER_YSON_STRUCT(TMaintenanceRequest);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TMaintenanceRequest)

////////////////////////////////////////////////////////////////////////////////

struct TTabletNodeInfo
    : public TYsonStructAttributes<TTabletNodeInfo>
{
    bool Banned;
    bool Decommissioned;
    bool DisableTabletCells;
    std::optional<bool> EnableBundleBalancer;
    TString Host;
    TString State;
    THashSet<TString> Tags;
    THashSet<TString> UserTags;
    TInstanceAnnotationsPtr Annotations;
    std::vector<TTabletSlotPtr> TabletSlots;
    THashMap<TString, TMaintenanceRequestPtr> MaintenanceRequests;
    TInstant LastSeenTime;

    REGISTER_YSON_STRUCT(TTabletNodeInfo);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TTabletNodeInfo)

////////////////////////////////////////////////////////////////////////////////

struct TRpcProxyAlive
    : public NYTree::TYsonStruct
{
    REGISTER_YSON_STRUCT(TRpcProxyAlive);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TRpcProxyAlive)

////////////////////////////////////////////////////////////////////////////////

struct TRpcProxyInfo
    : public TYsonStructAttributes<TRpcProxyInfo>
{
    bool Banned;
    TString Role;
    TInstanceAnnotationsPtr Annotations;
    THashMap<TString, TMaintenanceRequestPtr> MaintenanceRequests;

    TRpcProxyAlivePtr Alive;

    REGISTER_YSON_STRUCT(TRpcProxyInfo);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TRpcProxyInfo)

////////////////////////////////////////////////////////////////////////////////

struct TBundleDynamicConfig
    : public NYTree::TYsonStruct
{
    TCpuLimitsPtr CpuLimits;
    TMemoryLimitsPtr MemoryLimits;

    REGISTER_YSON_STRUCT(TBundleDynamicConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBundleDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

struct TAccountResources
    : public NYTree::TYsonStruct
{
    i64 ChunkCount;
    THashMap<TString, i64> DiskSpacePerMedium;
    i64 NodeCount;

    REGISTER_YSON_STRUCT(TAccountResources);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TAccountResources)

////////////////////////////////////////////////////////////////////////////////

struct TSystemAccount
    : public TYsonStructAttributes<TSystemAccount>
{
    TAccountResourcesPtr ResourceLimits;
    TAccountResourcesPtr ResourceUsage;

    REGISTER_YSON_STRUCT(TSystemAccount);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TSystemAccount)

////////////////////////////////////////////////////////////////////////////////

struct TBundleSystemOptions
    : public NYTree::TYsonStruct
{
    TString ChangelogAccount;
    TString ChangelogPrimaryMedium;

    TString SnapshotAccount;
    TString SnapshotPrimaryMedium;

    REGISTER_YSON_STRUCT(TBundleSystemOptions);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBundleSystemOptions)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellBalancer
