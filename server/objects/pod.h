#pragma once

#include "object.h"

#include <yp/server/objects/proto/objects.pb.h>

#include <yp/client/api/proto/data_model.pb.h>

#include <yt/core/misc/ref_tracked.h>

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

class TPod
    : public TObject
    , public NYT::TRefTracked<TPod>
{
public:
    static constexpr EObjectType Type = EObjectType::Pod;

    TPod(
        const TObjectId& id,
        const TObjectId& podSetId,
        IObjectTypeHandler* typeHandler,
        ISession* session);

    virtual EObjectType GetType() const override;

    using TPodSetAttribute = TParentAttribute<TPodSet>;
    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TPodSetAttribute, PodSet);

    class TStatus
    {
    public:
        explicit TStatus(TPod* pod);

        class TAgent
        {
        public:
            explicit TAgent(TPod* pod);

            static const TScalarAttributeSchema<TPod, EPodCurrentState> StateSchema;
            DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<EPodCurrentState>, State);

            static const TScalarAttributeSchema<TPod, TString> IssPayloadSchema;
            DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TString>, IssPayload);

            using TPodAgentPayload = NClient::NApi::NProto::TPodStatus_TAgent_TPodAgentPayload;
            static const TScalarAttributeSchema<TPod, TPodAgentPayload> PodAgentPayloadSchema;
            DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TPodAgentPayload>, PodAgentPayload);

            using TOther = NProto::TPodAgentStatusOther;
            static const TScalarAttributeSchema<TPod, TOther> OtherSchema;
            DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TOther>, Other);
        };

        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TAgent, Agent);

        static const TScalarAttributeSchema<TPod, ui64> GenerationNumberSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<ui64>, GenerationNumber);

        static const TScalarAttributeSchema<TPod, NTransactionClient::TTimestamp> AgentSpecTimestampSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<NTransactionClient::TTimestamp>, AgentSpecTimestamp);

        using TOther = NProto::TPodStatusOther;
        static const TScalarAttributeSchema<TPod, TOther> OtherSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TOther>, Other);
    };

    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TStatus, Status);

    class TSpec
    {
    public:
        explicit TSpec(TPod* pod);

        static const TManyToOneAttributeSchema<TPod, TNode> NodeSchema;
        using TNodeAttribute = TManyToOneAttribute<TPod, TNode>;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TNodeAttribute, Node);

        static const TScalarAttributeSchema<TPod, TString> IssPayloadSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TString>, IssPayload);

        using TPodAgentPayload = NClient::NApi::NProto::TPodSpec_TPodAgentPayload;
        static const TScalarAttributeSchema<TPod, TPodAgentPayload> PodAgentPayloadSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TPodAgentPayload>, PodAgentPayload);

        static const TScalarAttributeSchema<TPod, bool> EnableSchedulingSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<bool>, EnableScheduling);

        using TSecrets = THashMap<TString, NClient::NApi::NProto::TPodSpec_TSecret>;
        static const TScalarAttributeSchema<TPod, TSecrets> SecretsSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TSecrets>, Secrets);

        static const TTimestampAttributeSchema UpdateTimestampSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TTimestampAttribute, UpdateTimestamp);

        using TOther = NProto::TPodSpecOther;
        static const TScalarAttributeSchema<TPod, TOther> OtherSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TOther>, Other);

        static const TManyToOneAttributeSchema<TPod, TAccount> AccountSchema;
        using TAccountAttribute = TManyToOneAttribute<TPod, TAccount>;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TAccountAttribute, Account);
    };

    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TSpec, Spec);

    void UpdateEvictionStatus(
        EEvictionState state,
        EEvictionReason reason,
        const TString& message);

    void UpdateSchedulingStatus(
        ESchedulingState state,
        const TString& message,
        const TObjectId& nodeId = TObjectId());
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects
