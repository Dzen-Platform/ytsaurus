#include "pod.h"
#include "pod_set.h"
#include "node.h"
#include "account.h"
#include "db_schema.h"

#include <yt/core/misc/protobuf_helpers.h>

namespace NYP::NServer::NObjects {

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

const TScalarAttributeSchema<TPod, EPodCurrentState> TPod::TStatus::TAgent::StateSchema{
    &PodsTable.Fields.Status_Agent_State,
    [] (TPod* pod) { return &pod->Status().Agent().State(); }
};

const TScalarAttributeSchema<TPod, TString> TPod::TStatus::TAgent::IssPayloadSchema{
    &PodsTable.Fields.Status_Agent_IssPayload,
    [] (TPod* pod) { return &pod->Status().Agent().IssPayload(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TAgent::TPodAgentPayload> TPod::TStatus::TAgent::PodAgentPayloadSchema{
    &PodsTable.Fields.Status_Agent_PodAgentPayload,
    [] (TPod* pod) { return &pod->Status().Agent().PodAgentPayload(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TDynamicResourceStatus> TPod::TStatus::DynamicResourcesSchema{
    &PodsTable.Fields.Status_DynamicResources,
    [] (TPod* pod) { return &pod->Status().DynamicResources(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TAgent::TOther> TPod::TStatus::TAgent::OtherSchema{
    &PodsTable.Fields.Status_Agent_Other,
    [] (TPod* pod) { return &pod->Status().Agent().Other(); }
};

TPod::TStatus::TStatus::TAgent::TAgent(TPod* pod)
    : State_(pod, &StateSchema)
    , IssPayload_(pod, &IssPayloadSchema)
    , PodAgentPayload_(pod, &PodAgentPayloadSchema)
    , Other_(pod, &OtherSchema)
{ }

////////////////////////////////////////////////////////////////////////////////

const TScalarAttributeSchema<TPod, ui64> TPod::TStatus::GenerationNumberSchema{
    &PodsTable.Fields.Status_GenerationNumber,
    [] (TPod* pod) { return &pod->Status().GenerationNumber(); }
};

const TScalarAttributeSchema<TPod, NTransactionClient::TTimestamp> TPod::TStatus::AgentSpecTimestampSchema{
    &PodsTable.Fields.Status_AgentSpecTimestamp,
    [] (TPod* pod) { return &pod->Status().AgentSpecTimestamp(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TOther> TPod::TStatus::OtherSchema{
    &PodsTable.Fields.Status_Other,
    [] (TPod* pod) { return &pod->Status().Other(); }
};

TPod::TStatus::TStatus(TPod* pod)
    : Agent_(pod)
    , GenerationNumber_(pod, &GenerationNumberSchema)
    , AgentSpecTimestamp_(pod, &AgentSpecTimestampSchema)
    , DynamicResources_(pod, &DynamicResourcesSchema)
    , Other_(pod, &OtherSchema)
{ }

////////////////////////////////////////////////////////////////////////////////

const TManyToOneAttributeSchema<TPod, TNode> TPod::TSpec::NodeSchema{
    &PodsTable.Fields.Spec_NodeId,
    [] (TPod* pod) { return &pod->Spec().Node(); },
    [] (TNode* node) { return &node->Pods(); }
};

const TScalarAttributeSchema<TPod, TString> TPod::TSpec::IssPayloadSchema{
    &PodsTable.Fields.Spec_IssPayload,
    [] (TPod* pod) { return &pod->Spec().IssPayload(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TPodAgentPayload> TPod::TSpec::PodAgentPayloadSchema{
    &PodsTable.Fields.Spec_PodAgentPayload,
    [] (TPod* pod) { return &pod->Spec().PodAgentPayload(); }
};

const TScalarAttributeSchema<TPod, bool> TPod::TSpec::EnableSchedulingSchema{
    &PodsTable.Fields.Spec_EnableScheduling,
    [] (TPod* pod) { return &pod->Spec().EnableScheduling(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TSecrets> TPod::TSpec::SecretsSchema{
    &PodsTable.Fields.Spec_Secrets,
    [] (TPod* pod) { return &pod->Spec().Secrets(); }
};

const TTimestampAttributeSchema TPod::TSpec::UpdateTimestampSchema{
    &PodsTable.Fields.Spec_UpdateTag
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TDynamicResourceSpec> TPod::TSpec::DynamicResourcesSchema{
    &PodsTable.Fields.Spec_DynamicResources,
    [] (TPod* pod) { return &pod->Spec().DynamicResources(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TOther> TPod::TSpec::OtherSchema{
    &PodsTable.Fields.Spec_Other,
    [] (TPod* pod) { return &pod->Spec().Other(); }
};

const TManyToOneAttributeSchema<TPod, TAccount> TPod::TSpec::AccountSchema{
    &PodsTable.Fields.Spec_AccountId,
    [] (TPod* pod) { return &pod->Spec().Account(); },
    [] (TAccount* account) { return &account->Pods(); }
};

TPod::TSpec::TSpec(TPod* pod)
    : Node_(pod, &NodeSchema)
    , IssPayload_(pod, &IssPayloadSchema)
    , PodAgentPayload_(pod, &PodAgentPayloadSchema)
    , EnableScheduling_(pod, &EnableSchedulingSchema)
    , Secrets_(pod, &SecretsSchema)
    , UpdateTimestamp_(pod, &UpdateTimestampSchema)
    , DynamicResources_(pod, &DynamicResourcesSchema)
    , Other_(pod, &OtherSchema)
    , Account_(pod, &AccountSchema)
{ }

////////////////////////////////////////////////////////////////////////////////

TPod::TPod(
    const TObjectId& id,
    const TObjectId& podSetId,
    IObjectTypeHandler* typeHandler,
    ISession* session)
    : TObject(id, podSetId, typeHandler, session)
    , PodSet_(this)
    , Status_(this)
    , Spec_(this)
{ }

EObjectType TPod::GetType() const
{
    return EObjectType::Pod;
}

void TPod::UpdateEvictionStatus(
    EEvictionState state,
    EEvictionReason reason,
    const TString& message)
{
    auto* eviction = Status().Other()->mutable_eviction();
    eviction->set_state(static_cast<NClient::NApi::NProto::EEvictionState>(state));
    eviction->set_reason(static_cast<NClient::NApi::NProto::EEvictionReason>(reason));
    eviction->set_message(message);
    eviction->set_last_updated(ToProto<ui64>(TInstant::Now()));
}

void TPod::UpdateSchedulingStatus(
    ESchedulingState state,
    const TString& message,
    const TObjectId& nodeId)
{
    auto* scheduling = Status().Other()->mutable_scheduling();
    scheduling->set_state(static_cast<NClient::NApi::NProto::ESchedulingState>(state));
    scheduling->set_message(message);
    if (nodeId) {
        scheduling->set_node_id(nodeId);
    } else {
        scheduling->clear_node_id();
    }
    scheduling->set_last_updated(ToProto<ui64>(TInstant::Now()));
    scheduling->clear_error();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects

