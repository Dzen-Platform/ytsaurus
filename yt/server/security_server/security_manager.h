#pragma once

#include "public.h"
#include "cluster_resources.h"

#include <server/hydra/mutation.h>
#include <server/hydra/entity_map.h>

#include <core/rpc/service.h>

#include <server/cell_master/public.h>

#include <server/cypress_server/public.h>

#include <server/transaction_server/public.h>

#include <server/object_server/public.h>

#include <server/security_server/security_manager.pb.h>

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

    DECLARE_ENTITY_MAP_ACCESSORS(Account, TAccount, TAccountId);
    DECLARE_ENTITY_MAP_ACCESSORS(User, TUser, TUserId);
    DECLARE_ENTITY_MAP_ACCESSORS(Group, TGroup, TGroupId);

    NHydra::TMutationPtr CreateUpdateRequestStatisticsMutation(
        const NProto::TReqUpdateRequestStatistics& request);


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
    void ValidatePermission(
        NObjectServer::TObjectBase* object,
        TUser* user,
        EPermission permission);

    //! Another overload that uses the current user.
    void ValidatePermission(
        NObjectServer::TObjectBase* object,
        EPermission permission);


    //! Sets or resets banned flag for a given user.
    void SetUserBanned(TUser* user, bool banned);

    //! Checks if request handling is possible from a given user.
    /*!
     *  Enforces bans and rate limits.
     *  If successful, charges the user for a given number of requests.
     *  Throws on failure.
     */
    void ValidateUserAccess(TUser* user, int requestCount);

    //! Returns the current request rate from the user.
    double GetRequestRate(TUser* user);


private:
    class TImpl;
    class TAccountTypeHandler;
    class TUserTypeHandler;
    class TGroupTypeHandler;
    
    TIntrusivePtr<TImpl> Impl_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT
