#include "exec_node.h"

#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/ytlib/scheduler/job_resources.h>

namespace NYT::NScheduler {

using namespace NNodeTrackerClient;
using namespace NConcurrency;
using ENodeMasterState = NNodeTrackerServer::ENodeState;

////////////////////////////////////////////////////////////////////////////////

TExecNode::TExecNode(
    TNodeId id,
    const TNodeDescriptor& nodeDescriptor,
    ENodeState state)
    : Id_(id)
    , NodeDescriptor_(nodeDescriptor)
    , MasterState_(ENodeMasterState::Offline)
    , SchedulerState_(state)
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

    return TExecNodeDescriptor(
        Id_,
        GetDefaultAddress(),
        IOWeight_,
        MasterState_ == ENodeMasterState::Online && SchedulerState_ == ENodeState::Online,
        ResourceUsage_,
        ResourceLimits_,
        Tags_);
}

void TExecNode::SetIOWeights(const THashMap<TString, double>& mediumToWeight)
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


const NNodeTrackerClient::NProto::TDiskResources& TExecNode::GetDiskInfo() const
{
    return DiskInfo_;
}


void TExecNode::SetResourceUsage(const TJobResources& value)
{
    // NB: No locking is needed since ResourceUsage_ is not used
    // in BuildExecDescriptor.
    ResourceUsage_ = value;
}

void TExecNode::SetDiskInfo(const NNodeTrackerClient::NProto::TDiskResources& value)
{
    DiskInfo_ = value;
}

////////////////////////////////////////////////////////////////////////////////

TExecNodeDescriptor::TExecNodeDescriptor(
    NNodeTrackerClient::TNodeId id,
    const TString& address,
    double ioWeight,
    bool online,
    const TJobResources& resourceUsage,
    const TJobResources& resourceLimits,
    const THashSet<TString>& tags)
    : Id(id)
    , Address(address)
    , IOWeight(ioWeight)
    , Online(online)
    , ResourceUsage(resourceUsage)
    , ResourceLimits(resourceLimits)
    , Tags(tags)
{ }

bool TExecNodeDescriptor::CanSchedule(const TSchedulingTagFilter& filter) const
{
    return Online && (filter.IsEmpty() || filter.CanSchedule(Tags));
}

void TExecNodeDescriptor::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Id);
    Persist(context, Address);
    Persist(context, IOWeight);
    Persist(context, Online);
    Persist(context, ResourceLimits);
    Persist(context, Tags);
}

void ToProto(NScheduler::NProto::TExecNodeDescriptor* protoDescriptor, const NScheduler::TExecNodeDescriptor& descriptor)
{
    protoDescriptor->set_node_id(descriptor.Id);
    protoDescriptor->set_address(descriptor.Address);
    protoDescriptor->set_io_weight(descriptor.IOWeight);
    protoDescriptor->set_online(descriptor.Online);
    ToProto(protoDescriptor->mutable_resource_limits(), descriptor.ResourceLimits);
    for (const auto& tag : descriptor.Tags) {
        protoDescriptor->add_tags(tag);
    }
}

void FromProto(NScheduler::TExecNodeDescriptor* descriptor, const NScheduler::NProto::TExecNodeDescriptor& protoDescriptor)
{
    descriptor->Id = protoDescriptor.node_id();
    descriptor->Address = protoDescriptor.address();
    descriptor->IOWeight = protoDescriptor.io_weight();
    descriptor->Online = protoDescriptor.online();
    FromProto(&descriptor->ResourceLimits, protoDescriptor.resource_limits());
    for (const auto& tag : protoDescriptor.tags()) {
        descriptor->Tags.insert(tag);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

