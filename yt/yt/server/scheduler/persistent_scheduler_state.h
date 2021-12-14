#pragma once

#include "public.h"

#include <yt/yt/core/ytree/yson_serializable.h>

#include <yt/yt/library/vector_hdrf/job_resources.h>
#include <yt/yt/library/vector_hdrf/resource_volume.h>
#include <yt/yt/library/vector_hdrf/resource_helpers.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TPersistentPoolState
    : public NYTree::TYsonSerializable  // TODO(renadeen): Try to make it lite.
{
public:
    TResourceVolume AccumulatedResourceVolume;

    TPersistentPoolState();
};

DEFINE_REFCOUNTED_TYPE(TPersistentPoolState)

TString ToString(const TPersistentPoolStatePtr& state);
void FormatValue(TStringBuilderBase* builder, const TPersistentPoolStatePtr& state, TStringBuf /* format */);

////////////////////////////////////////////////////////////////////////////////

class TPersistentTreeState
    : public NYTree::TYsonSerializable
{
public:
    THashMap<TString, TPersistentPoolStatePtr> PoolStates;

    TPersistentTreeState();
};

DEFINE_REFCOUNTED_TYPE(TPersistentTreeState)

////////////////////////////////////////////////////////////////////////////////

class TPersistentStrategyState
    : public NYTree::TYsonSerializable
{
public:
    THashMap<TString, TPersistentTreeStatePtr> TreeStates;

    TPersistentStrategyState();
};

DEFINE_REFCOUNTED_TYPE(TPersistentStrategyState)

////////////////////////////////////////////////////////////////////////////////

struct TPersistentNodeSchedulingSegmentState
{
    ESchedulingSegment Segment;

    // NB: Used only for diagnostics.
    TString Address;
    TString Tree;
};

void Serialize(const TPersistentNodeSchedulingSegmentState& state, NYson::IYsonConsumer* consumer);
void Deserialize(TPersistentNodeSchedulingSegmentState& state, NYTree::INodePtr node);
void Deserialize(TPersistentNodeSchedulingSegmentState& state, NYson::TYsonPullParserCursor* cursor);

using TPersistentNodeSchedulingSegmentStateMap = THashMap<NNodeTrackerClient::TNodeId, TPersistentNodeSchedulingSegmentState>;

////////////////////////////////////////////////////////////////////////////////

class TPersistentSchedulingSegmentsState
    : public NYTree::TYsonSerializable
{
public:
    TPersistentNodeSchedulingSegmentStateMap NodeStates;

    TPersistentSchedulingSegmentsState();
};

DEFINE_REFCOUNTED_TYPE(TPersistentSchedulingSegmentsState)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
