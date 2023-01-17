#pragma once

#include "public.h"

#include "disk_location.h"

#include <yt/yt/library/containers/disk_manager/public.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

struct TLocationLivenessInfo
{
    TStoreLocationPtr Location;
    TString DiskId;
    ELocationState LocationState;
    bool IsDiskAlive;
};

////////////////////////////////////////////////////////////////////////////////

class TLocationManager
    : public TRefCounted
{
public:
    TLocationManager(
        TChunkStorePtr chunkStore,
        IInvokerPtr controlInvoker,
        NContainers::TDiskInfoProviderPtr diskInfoProvider);

    TFuture<std::vector<TLocationLivenessInfo>> GetLocationsLiveness();

    TFuture<std::vector<TGuid>> ResurrectChunkLocations(const THashSet<TGuid>& locationUuids);

    TFuture<std::vector<TGuid>> DisableChunkLocations(const THashSet<TGuid>& locationUuids);

    TFuture<std::vector<TGuid>> DestroyChunkLocations(const THashSet<TGuid>& locationUuids);

    TFuture<void> RecoverDisk(const TString& diskId);

private:
    const TChunkStorePtr ChunkStore_;
    const IInvokerPtr ControlInvoker_;
    const NContainers::TDiskInfoProviderPtr DiskInfoProvider_;

    std::vector<TLocationLivenessInfo> MapLocationToLivenessInfo(
        const std::vector<NContainers::TDiskInfo>& failedDisks);

    std::vector<TGuid> DoResurrectLocations(const THashSet<TGuid>& locationUuids);

    std::vector<TGuid> DoDisableLocations(const THashSet<TGuid>& locationUuids);

    std::vector<TGuid> DoDestroyLocations(const THashSet<TGuid>& locationUuids);

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
};

DEFINE_REFCOUNTED_TYPE(TLocationManager)

////////////////////////////////////////////////////////////////////////////////

class TLocationHealthChecker
    : public TRefCounted
{
public:
    TLocationHealthChecker(
        TLocationManagerPtr locationManager,
        IInvokerPtr invoker,
        TLocationHealthCheckerConfigPtr config);

    void Start();

    void OnHealthCheck();

    void OnDynamicConfigChanged(const TLocationHealthCheckerDynamicConfigPtr& newConfig);

private:
    const TLocationHealthCheckerConfigPtr Config_;
    bool Enabled_;

    const IInvokerPtr Invoker_;

    const TLocationManagerPtr LocationManager_;
    NConcurrency::TPeriodicExecutorPtr HealthCheckerExecutor_;
};

DEFINE_REFCOUNTED_TYPE(TLocationHealthChecker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
