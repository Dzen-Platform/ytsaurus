#pragma once

#include <yt/server/hydra/public.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/core/misc/public.h>

#include <yt/core/ytree/permission.h>

namespace NYT {
namespace NSecurityServer {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TClusterResources;
class TAccountStatistics;
class TRequestStatisticsUpdate;
class TUserStatistics;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

using NSecurityClient::TAccountId;
using NSecurityClient::TSubjectId;
using NSecurityClient::TUserId;
using NSecurityClient::TGroupId;

using NYTree::EPermission;
using NYTree::EPermissionSet;

using NSecurityClient::ESecurityAction;

DECLARE_ENTITY_TYPE(TAccount, TAccountId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TUser, TUserId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TGroup, TGroupId, NObjectClient::TDirectObjectIdHash)

class TSubject;

struct TAccessControlEntry;
struct TAccessControlList;
class TAccessControlDescriptor;

struct TPermissionCheckResult;

class TSecurityManagerConfig;
typedef TIntrusivePtr<TSecurityManagerConfig> TSecurityManagerConfigPtr;

class TSecurityManager;
typedef TIntrusivePtr<TSecurityManager> TSecurityManagerPtr;

class TRequestTracker;
typedef TIntrusivePtr<TRequestTracker> TRequestTrackerPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT
