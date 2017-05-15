#include "security_manager.h"
#include "private.h"
#include "config.h"
#include "tablet.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/ytlib/api/native_client.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/concurrency/fls.h>
#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/expiring_cache.h>

namespace NYT {
namespace NTabletNode {

using namespace NYTree;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NApi;
using namespace NSecurityClient;
using namespace NYPath;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TAuthenticatedUserGuard::TAuthenticatedUserGuard(
    TSecurityManagerPtr securityManager,
    const TNullable<Stroka>& maybeUser)
    : SecurityManager_(std::move(securityManager))
    , IsNull_(!maybeUser)
{
    if (!IsNull_) {
        SecurityManager_->SetAuthenticatedUser(*maybeUser);
    }
}

TAuthenticatedUserGuard::~TAuthenticatedUserGuard()
{
    if (!IsNull_) {
        SecurityManager_->ResetAuthenticatedUser();
    }
}

////////////////////////////////////////////////////////////////////////////////

struct TTablePermissionKey
{
    TObjectId TableId;
    Stroka User;
    EPermission Permission;

    // Hasher.
    operator size_t() const
    {
        size_t result = 0;
        HashCombine(result, TableId);
        HashCombine(result, User);
        HashCombine(result, Permission);
        return result;
    }

    // Comparer.
    bool operator == (const TTablePermissionKey& other) const
    {
        return
            TableId == other.TableId &&
            User == other.User &&
            Permission == other.Permission;
    }

    // Formatter.
    friend Stroka ToString(const TTablePermissionKey& key)
    {
        return Format("%v:%v:%v",
            key.TableId,
            key.User,
            key.Permission);
    }
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TTablePermissionCache)

class TTablePermissionCache
    : public TExpiringCache<TTablePermissionKey, void>
{
public:
    TTablePermissionCache(
        TExpiringCacheConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : TExpiringCache(std::move(config))
        , Bootstrap_(bootstrap)
    { }

private:
    NCellNode::TBootstrap* const Bootstrap_;

    virtual TFuture<void> DoGet(const TTablePermissionKey& key) override
    {
        LOG_DEBUG("Table permission check started (Key: %v)",
            key);

        auto client = Bootstrap_->GetMasterClient();
        auto options = TCheckPermissionOptions();
        options.ReadFrom = EMasterChannelKind::Cache;
        return client->CheckPermission(key.User, FromObjectId(key.TableId), key.Permission, options).Apply(
            BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TCheckPermissionResult>& resultOrError) {
                if (!resultOrError.IsOK()) {
                    auto wrappedError = TError("Error checking permission for table %v",
                        key.TableId)
                        << resultOrError;
                    LOG_WARNING(wrappedError);
                    THROW_ERROR wrappedError;
                }

                const auto& result = resultOrError.Value();

                LOG_DEBUG("Table permission check complete (Key: %v, Action: %v)",
                    key,
                    result.Action);

                auto error = result.ToError(key.User, key.Permission);
                if (!error.IsOK()) {
                    THROW_ERROR error << TErrorAttribute("object", key.TableId);
                }
            }));
    }
};

DEFINE_REFCOUNTED_TYPE(TTablePermissionCache)

////////////////////////////////////////////////////////////////////////////////

struct TResourceLimitsKey
{
    Stroka Account;
    Stroka MediumName;

    // Hasher.
    operator size_t() const
    {
        size_t result = 0;
        HashCombine(result, Account);
        HashCombine(result, MediumName);
        return result;
    }

    // Comparer.
    bool operator == (const TResourceLimitsKey& other) const
    {
        return
            Account == other.Account &&
            MediumName == other.MediumName;
    }

    // Formatter.
    friend Stroka ToString(const TResourceLimitsKey& key)
    {
        return Format("%v:%v",
            key.Account,
            key.MediumName);
    }
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TResourceLimitsCache)

class TResourceLimitsCache
    : public TExpiringCache<TResourceLimitsKey, void>
{
public:
    TResourceLimitsCache(
        TExpiringCacheConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : TExpiringCache(std::move(config))
        , Bootstrap_(bootstrap)
    { }

private:
    NCellNode::TBootstrap* const Bootstrap_;

    virtual TFuture<void> DoGet(const TResourceLimitsKey& key) override
    {
        auto client = Bootstrap_->GetMasterClient();
        auto options = TGetNodeOptions();
        options.ReadFrom = EMasterChannelKind::Cache;
        return client->GetNode("//sys/accounts/" + ToYPathLiteral(key.Account) + "/@violated_resource_limits", options).Apply(
            BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TYsonString>& resultOrError) {
                if (!resultOrError.IsOK()) {
                    auto wrappedError = TError("Error getting resource limits for account %Qv",
                        key.Account)
                        << resultOrError;
                    LOG_WARNING(wrappedError);
                    THROW_ERROR wrappedError;
                }

                const auto& node = ConvertToNode(resultOrError.Value());

                LOG_DEBUG("Got resource limits violations for account %Qv: %Qv",
                    key.Account,
                    ConvertToYsonString(node, EYsonFormat::Text));

                if (node->AsMap()->GetChild("chunk_count")->GetValue<bool>()) {
                    THROW_ERROR_EXCEPTION("Account %Qv violates chunk count limit",
                        key.Account);
                }

                const auto& mediumLimit = node->AsMap()->GetChild("disk_space_per_medium")->AsMap()->FindChild(key.MediumName);

                if (!mediumLimit) {
                    THROW_ERROR_EXCEPTION("Unknown medium %Qv",
                        key.MediumName);
                }
                if (mediumLimit->GetValue<bool>()) {
                    THROW_ERROR_EXCEPTION("Account %Qv violates disk space limit for medium %Qv",
                        key.Account,
                        key.MediumName);
                }
            }));
    }
};

DEFINE_REFCOUNTED_TYPE(TResourceLimitsCache)

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TSecurityManagerConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(std::move(config))
        , Bootstrap_(bootstrap)
        , TablePermissionCache_(New<TTablePermissionCache>(Config_->TablePermissionCache, Bootstrap_))
        , ResourceLimitsCache_(New<TResourceLimitsCache>(Config_->ResourceLimitsCache, Bootstrap_))
    { }

    void SetAuthenticatedUser(const Stroka& user)
    {
        Y_ASSERT(!*AuthenticatedUser_);
        *AuthenticatedUser_ = user;
    }

    void ResetAuthenticatedUser()
    {
        Y_ASSERT(*AuthenticatedUser_);
        AuthenticatedUser_->Reset();
    }

    TNullable<Stroka> GetAuthenticatedUser()
    {
        return *AuthenticatedUser_;
    }

    TFuture<void> CheckPermission(
        const TTabletSnapshotPtr& tabletSnapshot,
        EPermission permission)
    {
        auto maybeUser = GetAuthenticatedUser();
        if (!maybeUser) {
            return VoidFuture;
        }

        //if (tabletSnapshot->ReplicationMode == NTableClient::ETableReplicationMode::AsynchronousSink &&
        //    permission == EPermission::Write &&
        //    *maybeUser != NSecurityClient::ReplicatorUserName)
        //{
        //    THROW_ERROR_EXCEPTION("Only %Qv is allowed to write into tables with %Qlv replication mode",
        //        NSecurityClient::ReplicatorUserName,
        //        tabletSnapshot->ReplicationMode);
        //}
        //
        TTablePermissionKey key{tabletSnapshot->TableId, *maybeUser, permission};
        return TablePermissionCache_->Get(key);
    }

    void ValidatePermission(
        const TTabletSnapshotPtr& tabletSnapshot,
        EPermission permission)
    {
        auto asyncResult = CheckPermission(std::move(tabletSnapshot), permission);
        auto maybeResult = asyncResult.TryGet();
        auto result = maybeResult ? *maybeResult : WaitFor(asyncResult);
        result.ThrowOnError();
    }

    TFuture<void> CheckResourceLimits(
        const Stroka& account,
        const Stroka& mediumName)
    {
        return ResourceLimitsCache_->Get(TResourceLimitsKey{account, mediumName});
    }

    void ValidateResourceLimits(
        const Stroka& account,
        const Stroka& mediumName)
    {
        auto asyncResult = CheckResourceLimits(account, mediumName);
        auto maybeResult = asyncResult.TryGet();
        auto result = maybeResult ? *maybeResult : WaitFor(asyncResult);
        result.ThrowOnError();
    }

private:
    const TSecurityManagerConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    const TTablePermissionCachePtr TablePermissionCache_;
    const TResourceLimitsCachePtr ResourceLimitsCache_;

    TFls<TNullable<Stroka>> AuthenticatedUser_;

};

////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TSecurityManager(
    TSecurityManagerConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        std::move(config),
        bootstrap))
{ }

TSecurityManager::~TSecurityManager() = default;

void TSecurityManager::SetAuthenticatedUser(const Stroka& user)
{
    Impl_->SetAuthenticatedUser(user);
}

void TSecurityManager::ResetAuthenticatedUser()
{
    Impl_->ResetAuthenticatedUser();
}

TNullable<Stroka> TSecurityManager::GetAuthenticatedUser()
{
    return Impl_->GetAuthenticatedUser();
}

TFuture<void> TSecurityManager::CheckPermission(
    const TTabletSnapshotPtr& tabletSnapshot,
    EPermission permission)
{
    return Impl_->CheckPermission(std::move(tabletSnapshot), permission);
}

void TSecurityManager::ValidatePermission(
    const TTabletSnapshotPtr& tabletSnapshot,
    EPermission permission)
{
    Impl_->ValidatePermission(std::move(tabletSnapshot), permission);
}

TFuture<void> TSecurityManager::CheckResourceLimits(
    const Stroka& account,
    const Stroka& mediumName)
{
    return Impl_->CheckResourceLimits(account, mediumName);
}

void TSecurityManager::ValidateResourceLimits(
    const Stroka& account,
    const Stroka& mediumName)
{
    Impl_->ValidateResourceLimits(account, mediumName);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
