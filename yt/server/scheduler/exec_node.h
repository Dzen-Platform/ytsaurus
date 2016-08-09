#pragma once

#include "public.h"
#include "job_resources.h"

#include <yt/server/node_tracker_server/node.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/scheduler/scheduler_service.pb.h>

#include <yt/core/concurrency/lease_manager.h>
#include <yt/core/misc/property.h>

#include <yt/core/concurrency/rw_spinlock.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

//! Scheduler-side representation of an execution node.
/*!
 *  Thread affinity: ControlThread (unless noted otherwise)
 */
class TExecNode
    : public TIntrinsicRefCounted
{
private:
    typedef yhash_map<TJobId, TJobPtr> TJobMap;

public:
    DEFINE_BYVAL_RO_PROPERTY(NNodeTrackerClient::TNodeId, Id);
    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::TNodeDescriptor, NodeDescriptor);

    //! Jobs that are currently running on this node.
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJobPtr>, Jobs);

    //! Mapping from job id to job on this node.
    DEFINE_BYREF_RW_PROPERTY(TJobMap, IdToJob);

    //! A set of scheduling tags assigned to this node.
    DEFINE_BYREF_RW_PROPERTY(yhash_set<Stroka>, Tags);

    //! Last time when logging of jobs on node took place.
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, LastJobsLogTime);

    //! Last time when statistics and resource usage from running jobs was updated.
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, LastRunningJobsUpdateTime);

    //! Last time when missing jobs were checked on this node.
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, LastCheckMissingJobsTime);

    //! Last time when heartbeat from node was processed.
    DEFINE_BYVAL_RW_PROPERTY(TInstant, LastSeenTime);

    //! Controls heartbeat expiration.
    DEFINE_BYVAL_RW_PROPERTY(NConcurrency::TLease, Lease);

    //! State of node at master.
    DEFINE_BYVAL_RW_PROPERTY(NNodeTrackerServer::ENodeState, MasterState);

    //! Is |true| iff heartbeat from this node is being processed at the moment.
    DEFINE_BYVAL_RW_PROPERTY(bool, HasOngoingHeartbeat);

    //! Is |true| iff jobs are scheduled on the node at the moment by the strategy.
    DEFINE_BYVAL_RW_PROPERTY(bool, HasOngoingJobsScheduling);

    //! Is |true| iff the node must be unregistered but it also has an ongoing
    //! heartbeat so the unregistration has to be postponed until the heartbeat processing
    //! is complete.
    DEFINE_BYVAL_RW_PROPERTY(bool, HasPendingUnregistration);

public:
    TExecNode(
        NNodeTrackerClient::TNodeId id,
        const NNodeTrackerClient::TNodeDescriptor& nodeDescriptor);

    const Stroka& GetDefaultAddress() const;

    //! Checks if the node can handle jobs demanding a certain #tag.
    bool CanSchedule(const TNullable<Stroka>& tag) const;

    //! Constructs a descriptor containing the current snapshot of node's state.
    /*!
     *  Thread affinity: any
     */
    TExecNodeDescriptor BuildExecDescriptor() const;

    //! Returns the node's IO weight, as reported by node to master.
    double GetIOWeight() const;

    //! Set the node's IO weight.
    void SetIOWeight(double value);

    //! Returns the node's resource limits, as reported by the node.
    const TJobResources& GetResourceLimits() const;

    //! Sets the node's resource limits.
    void SetResourceLimits(const TJobResources& value);

    //! Returns the most recent resource usage, as reported by the node.
    /*!
     *  Some fields are also updated by the scheduler strategy to
     *  reflect recent job set changes.
     *  E.g. when the scheduler decides to
     *  start a new job it decrements the appropriate counters.
     */
    const TJobResources& GetResourceUsage() const;

    //! Sets the node's resource usage.
    void SetResourceUsage(const TJobResources& value);

private:
    TJobResources ResourceUsage_;

    mutable NConcurrency::TReaderWriterSpinLock SpinLock_;
    TJobResources ResourceLimits_;
    double IOWeight_ = 0;
};

DEFINE_REFCOUNTED_TYPE(TExecNode)

////////////////////////////////////////////////////////////////////////////////

//! An immutable snapshot of TExecNode.
struct TExecNodeDescriptor
{
    TExecNodeDescriptor();

    TExecNodeDescriptor(
        NNodeTrackerClient::TNodeId id,
        Stroka address,
        double ioWeight,
        TJobResources resourceLimits);

    NNodeTrackerClient::TNodeId Id = NNodeTrackerClient::InvalidNodeId;
    Stroka Address;
    double IOWeight = 0.0;
    TJobResources ResourceLimits;

    void Persist(const TStreamPersistenceContext& context);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
