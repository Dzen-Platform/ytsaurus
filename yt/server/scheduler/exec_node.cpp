#include "exec_node.h"
#include "job_resources.h"

#include <yt/ytlib/node_tracker_client/helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient;
using namespace NNodeTrackerServer;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TExecNode::TExecNode(
    TNodeId id,
    const TNodeDescriptor& nodeDescriptor)
    : Id_(id)
    , NodeDescriptor_(nodeDescriptor)
    , MasterState_(ENodeState::Offline)
    , HasOngoingHeartbeat_(false)
    , HasOngoingJobsScheduling_(false)
    , HasPendingUnregistration_(false)
{ }

const TString& TExecNode::GetDefaultAddress() const
{
    return NodeDescriptor_.GetDefaultAddress();
}

bool TExecNode::CanSchedule(const TSchedulingTagFilter& filter) const
{
    return filter.IsEmpty() || filter.CanSchedule(Tags_);
}

TExecNodeDescriptor TExecNode::BuildExecDescriptor() const
{
    TReaderGuard guard(SpinLock_);
    return TExecNodeDescriptor{
        Id_,
        GetDefaultAddress(),
        IOWeight_,
        ResourceLimits_,
        Tags_
    };
}

double TExecNode::GetIOWeight() const
{
    return IOWeight_;
}

void TExecNode::SetIOWeights(const yhash<TString, double>& mediumToWeight)
{
    TWriterGuard guard(SpinLock_);
    // NB: surely, something smarter than this should be done with individual medium weights here.
    IOWeight_ = 0.0;
    for (const auto& pair : mediumToWeight) {
        IOWeight_ += pair.second;
    }
}

const TJobResources& TExecNode::GetResourceLimits() const
{
    return ResourceLimits_;
}

void TExecNode::SetResourceLimits(const TJobResources& value)
{
    TWriterGuard guard(SpinLock_);
    ResourceLimits_ = value;
}

const TJobResources& TExecNode::GetResourceUsage() const
{
    return ResourceUsage_;
}

void TExecNode::SetResourceUsage(const TJobResources& value)
{
    // NB: No locking is needed since ResourceUsage_ is not used
    // in BuildExecDescriptor.
    ResourceUsage_ = value;
}

////////////////////////////////////////////////////////////////////////////////

TExecNodeDescriptor::TExecNodeDescriptor(
    NNodeTrackerClient::TNodeId id,
    const TString& address,
    double ioWeight,
    const TJobResources& resourceLimits,
    const yhash_set<TString>& tags)
    : Id(id)
    , Address(address)
    , IOWeight(ioWeight)
    , ResourceLimits(resourceLimits)
    , Tags(tags)
{ }

bool TExecNodeDescriptor::CanSchedule(const TSchedulingTagFilter& filter) const
{
    return filter.IsEmpty() || filter.CanSchedule(Tags);
}

void TExecNodeDescriptor::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Id);
    Persist(context, Address);
    Persist(context, IOWeight);
    Persist(context, ResourceLimits);
    Persist(context, Tags);
}

////////////////////////////////////////////////////////////////////////////////

TJobNodeDescriptor::TJobNodeDescriptor(const TExecNodeDescriptor& other)
    : Id(other.Id)
    , Address(other.Address)
    , IOWeight(other.IOWeight)
{ }

void TJobNodeDescriptor::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Id);
    Persist(context, Address);
    Persist(context, IOWeight);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

