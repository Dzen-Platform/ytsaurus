#pragma once

#include "public.h"

#include <yt/client/tablet_client/public.h>

#include <yt/core/misc/optional.h>

namespace NYT::NSecurityServer {

////////////////////////////////////////////////////////////////////////////////

//! A simple RAII guard for setting the authenticated user.
/*!
 *  \see #TSecurityManagerBase::SetAuthenticatedUserByName
 *  \see #TSecurityManagerBase::ResetAuthenticatedUser
 */
class TAuthenticatedUserGuardBase
    : private TNonCopyable
{
public:
    TAuthenticatedUserGuardBase(IUsersManagerPtr securityManager, const std::optional<TString>& userName);
    ~TAuthenticatedUserGuardBase();

protected:
    IUsersManagerPtr SecurityManager_;
};

////////////////////////////////////////////////////////////////////////////////

struct IUsersManager
    : public virtual TRefCounted
{
    //! Sets the authenticated user by user name.
    virtual void SetAuthenticatedUserByNameOrThrow(const TString& userName) = 0;

    //! Resets the authenticated user.
    virtual void ResetAuthenticatedUser() = 0;

    //! Returns the current user or null if there's no one.
    virtual std::optional<TString> GetAuthenticatedUserName() = 0;
};

DEFINE_REFCOUNTED_TYPE(IUsersManager)

////////////////////////////////////////////////////////////////////////////////

struct IResourceLimitsManager
    : public virtual TRefCounted
{
    virtual void ValidateResourceLimits(
        const TString& account,
        const TString& mediumName,
        NTabletClient::EInMemoryMode inMemoryMode) = 0;
};

DEFINE_REFCOUNTED_TYPE(IResourceLimitsManager)

/////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityServer
