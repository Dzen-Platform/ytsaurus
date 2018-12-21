#pragma once

#include <yt/core/misc/public.h>

namespace NYP::NClient::NApi {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TPodSpec_THostDevice;
class TPodSpec_TSysctlProperty;

} // namespace NProto

using TObjectId = TString;
using TTransactionId = NYT::TGuid;

constexpr int MaxObjectIdLength = 256;
constexpr int MaxNodeShortNameLength = 250;
constexpr int MaxPodFqdnLength = 630;

DEFINE_ENUM(EErrorCode,
    ((InvalidObjectId)             (100000))
    ((DuplicateObjectId)           (100001))
    ((NoSuchObject)                (100002))
    ((NotEnoughResources)          (100003))
    ((InvalidObjectType)           (100004))
    ((AuthenticationError)         (100005))
    ((AuthorizationError)          (100006))
    ((InvalidTransactionState)     (100007))
    ((InvalidTransactionId)        (100008))
    ((InvalidObjectState)          (100009))
    ((NoSuchTransaction)           (100010))
    ((UserBanned)                  (100011))
    ((AccountLimitExceeded)        (100012))
    ((PodSchedulingFailure)        (100013))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NClient::NApi
