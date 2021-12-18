#pragma once

#include "public.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>

#include <yt/yt/server/node/data_node/public.h>

#include <yt/yt/server/node/job_agent/job.h>

#include <yt/yt/ytlib/scheduler/proto/job.pb.h>

#include <yt/yt/core/actions/public.h>

#include <yt/yt/core/concurrency/public.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/fs.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NExecNode {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ESlotManagerAlertType,
    ((GenericPersistentError)         (0))
    ((GpuCheckFailed)                 (1))
    ((TooManyConsecutiveJobAbortions) (2))
    ((JobProxyUnavailable)            (3))
)

////////////////////////////////////////////////////////////////////////////////

//! Controls acquisition and release of slots.
/*!
 *  \note
 *  Thread affinity: Job (unless noted otherwise)
 */
class TSlotManager
    : public TRefCounted
{
public:
    TSlotManager(
        TSlotManagerConfigPtr config,
        IBootstrap* bootstrap);

    //! Initializes slots etc.
    void Initialize();

    void OnDynamicConfigChanged(
        const NClusterNode::TClusterNodeDynamicConfigPtr& oldNodeConfig,
        const NClusterNode::TClusterNodeDynamicConfigPtr& newNodeConfig);

    //! Acquires a free slot, thows on error.
    ISlotPtr AcquireSlot(NScheduler::NProto::TDiskRequest diskRequest);

    class TSlotGuard
    {
    public:
        explicit TSlotGuard(TSlotManagerPtr slotManager);
        ~TSlotGuard();

        int GetSlotIndex() const;

    private:
        const TSlotManagerPtr SlotManager_;
        const int SlotIndex_;
    };
    std::unique_ptr<TSlotGuard> AcquireSlot();

    int GetSlotCount() const;
    int GetUsedSlotCount() const;

    bool IsInitialized() const;
    bool IsEnabled() const;
    bool HasFatalAlert() const;

    NNodeTrackerClient::NProto::TDiskResources GetDiskResources();

    /*!
     *  \note
     *  Thread affinity: any
     */
    std::vector<TSlotLocationPtr> GetLocations() const;

    /*!
     *  \note
     *  Thread affinity: any
     */
    void Disable(const TError& error);

    /*!
     *  \note
     *  Thread affinity: any
     */
    void OnGpuCheckCommandFailed(const TError& error);

    /*!
     *  \note
     *  Thread affinity: any
     */
    void BuildOrchidYson(NYTree::TFluentMap fluent) const;

    /*!
     *  \note
     *  Thread affinity: any
     */
    void InitMedia(const NChunkClient::TMediumDirectoryPtr& mediumDirectory);

private:
    const TSlotManagerConfigPtr Config_;
    IBootstrap* const Bootstrap_;
    const int SlotCount_;
    const TString NodeTag_;

    std::atomic_bool Initialized_ = false;
    std::atomic_bool JobProxyReady_ = false;

    TAtomicObject<TSlotManagerDynamicConfigPtr> DynamicConfig_;

    IVolumeManagerPtr RootVolumeManager_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, LocationsLock_);
    std::vector<TSlotLocationPtr> Locations_;
    std::vector<TSlotLocationPtr> AliveLocations_;

    IJobEnvironmentPtr JobEnvironment_;

    THashSet<int> FreeSlots_;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);
    TEnumIndexedVector<ESlotManagerAlertType, TError> Alerts_;
    //! If we observe too many consecutive aborts, we disable user slots on
    //! the node until restart and fire alert.
    int ConsecutiveAbortedJobCount_ = 0;

    int DefaultMediumIndex_ = NChunkClient::DefaultSlotsMediumIndex;

    DECLARE_THREAD_AFFINITY_SLOT(JobThread);

    bool HasSlotDisablingAlert() const;

    void AsyncInitialize();

    int DoAcquireSlot();
    void ReleaseSlot(int slotIndex);

    /*!
     *  \note
     *  Thread affinity: any
     */
    void OnJobFinished(const NJobAgent::IJobPtr& job);

    /*!
     *  \note
     *  Thread affinity: any
     */
    void OnJobProxyBuildInfoUpdated(const TError& error);

    void OnJobsCpuLimitUpdated();
    void UpdateAliveLocations();
    void ResetConsecutiveAbortedJobCount();
    void PopulateAlerts(std::vector<TError>* alerts);
};

DEFINE_REFCOUNTED_TYPE(TSlotManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
