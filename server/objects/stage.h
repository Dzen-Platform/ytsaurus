#pragma once

#include "object.h"

#include <yp/server/objects/proto/autogen.pb.h>

#include <yt/core/misc/ref_tracked.h>
#include <yt/core/misc/property.h>

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

class TStage
    : public TObject
    , public NYT::TRefTracked<TStage>
{
public:
    static constexpr EObjectType Type = EObjectType::Stage;

    TStage(
        const TObjectId& id,
        IObjectTypeHandler* typeHandler,
        ISession* session);

    virtual EObjectType GetType() const override;

    class TSpec
    {
    public:
        explicit TSpec(TStage* stage);

        static const TManyToOneAttributeSchema<TStage, TAccount> AccountSchema;
        using TAccountAttribute = TManyToOneAttribute<TStage, TAccount>;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TAccountAttribute, Account);

        using TEtc = NProto::TStageSpecEtc;
        static const TScalarAttributeSchema<TStage, TEtc> EtcSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TEtc>, Etc);
    };
    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TSpec, Spec);

    using TStatus = NYP::NClient::NApi::NProto::TStageStatus;
    static const TScalarAttributeSchema<TStage, TStatus> StatusSchema;
    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TStatus>, Status);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects
