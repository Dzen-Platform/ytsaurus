#pragma once

#include "public.h"
#include "cluster_resources.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/cypress_server/public.h>

#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/mutation.h>

#include <yt/server/object_server/public.h>

#include <yt/server/security_server/security_manager.pb.h>

#include <yt/server/transaction_server/public.h>

#include <yt/core/rpc/service.h>

namespace NYT {
namespace NSecurityServer {

////////////////////////////////////////////////////////////////////////////////

//! Describes the result of #TSecurityManager::CheckAccess invocation.
struct TPermissionCheckResult
{
    //! Was request allowed or declined?
    ESecurityAction Action = ESecurityAction::Undefined;

    //! The object whose ACL contains the matching ACE.
    //! May be |nullptr| if check fails due to missing ACE or succeeds because the user is "root".
    NObjectServer::TObjectBase* Object = nullptr;

    //! Subject to which the decision applies.
    //! Can be |nullptr| if check fails due to missing ACE or succeeds because the user is "root".
    TSubject* Subject = nullptr;
};

////////////////////////////////////////////////////////////////////////////////

//! A simple RAII guard for setting the current authenticated user.
/*!
 *  \see #TSecurityManager::SetAuthenticatedUser
 *  \see #TSecurityManager::ResetAuthenticatedUser
 */
class TAuthenticatedUserGuard
    : private TNonCopyable
{
public:
    TAuthenticatedUserGuard(TSecurityManagerPtr securityManager, TUser* user);
    ~TAuthenticatedUserGuard();

private:
    const TSecurityManagerPtr SecurityManager_;
    const bool IsNull_;

};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager
    : public TRefCounted
{
public:
    TSecurityManager(
        TSecurityManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);
    ~TSecurityManager();

    void Initialize();

    DECLARE_ENTITY_MAP_ACCESSORS(Account, TAccount);
    DECLARE_ENTITY_MAP_ACCESSORS(User, TUser);
    DECLARE_ENTITY_MAP_ACCESSORS(Group, TGroup);

    //! Returns account with a given name (|nullptr| if none).
    TAccount* FindAccountByName(const Stroka& name);

    //! Returns user with a given name (throws if none).
    TAccount* GetAccountByNameOrThrow(const Stroka& name);

    //! Returns "root" built-in account.
    TAccount* GetSysAccount();

    //! Returns "tmp" built-in account.
    TAccount* GetTmpAccount();

    //! Return "intermediate" built-in account.
    TAccount* GetIntermediateAccount();


    //! Assigns node to a given account, updates the total resource usage.
    void SetAccount(NCypressServer::TCypressNodeBase* node, TAccount* account);

    //! Removes account association (if any) from the node.
    void ResetAccount(NCypressServer::TCypressNodeBase* node);

    //! Updates the name of the account.
    void RenameAccount(TAccount* account, const Stroka& newName);

    //! Updates the account to accommodate recent changes in #node resource usage.
    void UpdateAccountNodeUsage(NCypressServer::TCypressNodeBase* node);

    //! Enables or disables accounting for a given node updating account usage appropriately.
    void SetNodeResourceAccounting(NCypressServer::TCypressNodeBase* node, bool enable);

    //! Updates the staging resource usage for a given account.
    void UpdateAccountStagingUsage(
        NTransactionServer::TTransaction* transaction,
        TAccount* account,
        const TClusterResources& delta);


    //! Returns user with a given name (|nullptr| if none).
    TUser* FindUserByName(const Stroka& name);

    //! Returns user with a given name (throws if none).
    TUser* GetUserByNameOrThrow(const Stroka& name);

    //! Finds user by id, throws if nothing is found.
    TUser* GetUserOrThrow(const TUserId& id);

    //! Returns "root" built-in user.
    TUser* GetRootUser();

    //! Returns "guest" built-in user.
    TUser* GetGuestUser();


    //! Returns group with a given name (|nullptr| if none).
    TGroup* FindGroupByName(const Stroka& name);

    //! Returns "everyone" built-in group.
    TGroup* GetEveryoneGroup();

    //! Returns "users" built-in group.
    TGroup* GetUsersGroup();

    //! Returns "superusers" built-in group.
    TGroup* GetSuperusersGroup();


    //! Returns subject (a user or a group) with a given name (|nullptr| if none).
    TSubject* FindSubjectByName(const Stroka& name);

    //! Returns subject (a user or a group) with a given name (throws if none).
    TSubject* GetSubjectByNameOrThrow(const Stroka& name);

    //! Adds a new member into the group. Throws on failure.
    void AddMember(TGroup* group, TSubject* member);

    //! Removes an existing member from the group. Throws on failure.
    void RemoveMember(TGroup* group, TSubject* member);


    //! Updates the name of the subject.
    void RenameSubject(TSubject* subject, const Stroka& newName);


    //! Returns the set of supported permissions.
    EPermissionSet GetSupportedPermissions(NObjectServer::TObjectBase* object);

    //! Returns the object ACD or |nullptr| if access is not controlled.
    TAccessControlDescriptor* FindAcd(NObjectServer::TObjectBase* object);

    //! Returns the object ACD. Fails if no ACD exists.
    TAccessControlDescriptor* GetAcd(NObjectServer::TObjectBase* object);

    //! Returns the ACL obtained by combining ACLs of the object and its parents.
    //! The returned ACL is a fake one, i.e. does not exist explicitly anywhere.
    TAccessControlList GetEffectiveAcl(NObjectServer::TObjectBase* object);


    //! Sets the authenticated user.
    void SetAuthenticatedUser(TUser* user);

    //! Resets the authenticated user.
    void ResetAuthenticatedUser();

    //! Returns the current user or |nullptr| if there's no one.
    TUser* GetAuthenticatedUser();


    //! Checks if #object ACL allows access with #permission.
    TPermissionCheckResult CheckPermission(
        NObjectServer::TObjectBase* object,
        TUser* user,
        EPermission permission);

    //! Similar to #CheckPermission but throws a human-readable exception on failure.
    /*!
     *  If NHiveServer::IsHiveMutation returns |true| then this check is suppressed.
     */
    void ValidatePermission(
        NObjectServer::TObjectBase* object,
        TUser* user,
        EPermission permission);

    //! Another overload that uses the current user.
    void ValidatePermission(
        NObjectServer::TObjectBase* object,
        EPermission permission);


    //! Throws if account limit is exceeded for some resource type with positive delta.
    /*!
     *  If NHive::IsHiveMutation returns |true| then this check is suppressed.
     */
    void ValidateResourceUsageIncrease(TAccount* account, const TClusterResources& delta);


    //! Sets or resets banned flag for a given user.
    void SetUserBanned(TUser* user, bool banned);

    //! Checks if request handling is possible from a given user.
    /*!
     *  Throws if the user is banned.
     */
    void ValidateUserAccess(TUser* user);

    //! Increments per-user counters.
    void ChargeUserRead(
        TUser* user,
        int requestCount,
        TDuration time);

    //! The behavior differs at leaders and at followers:
    //! 1) At leaders, this increments per-user counters.
    //! 2) At followers, no counters are incremented (the leader is responsible for this) but
    //! the request rate throttler is acquired unconditionally.
    void ChargeUserWrite(
        TUser* user,
        int requestCount,
        TDuration time);

    //! Enforces request rate limits.
    TFuture<void> ThrottleUser(
        TUser* user,
        int requestCount);

    //! Updates the user request rate limit.
    void SetUserRequestRateLimit(TUser* user, int limit);

    //! Updates the user request queue size limit.
    void SetUserRequestQueueSizeLimit(TUser* user, int limit);

    //! Attempts to increase the queue size for a given #user and validates the limit.
    //! Returns |true| on success.
    bool TryIncreaseRequestQueueSize(TUser* user);

    //! Unconditionally decreases the queue size for a given #user.
    void DecreaseRequestQueueSize(TUser* user);

private:
    class TImpl;
    class TAccountTypeHandler;
    class TUserTypeHandler;
    class TGroupTypeHandler;
    
    const TIntrusivePtr<TImpl> Impl_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT
