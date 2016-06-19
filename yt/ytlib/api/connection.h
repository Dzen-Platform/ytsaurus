#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/core/actions/callback.h>

#include <yt/core/rpc/public.h>

#include <yt/ytlib/object_client/public.h>

namespace NYT {
namespace NApi {

////////////////////////////////////////////////////////////////////////////////

struct TAdminOptions
{ };

struct TClientOptions
{
    explicit TClientOptions(const Stroka& user =  NSecurityClient::GuestUserName)
        : User(user)
    { }

    Stroka User;
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMasterChannelKind,
    (Leader)
    (Follower)
    (LeaderOrFollower)
    (Cache)
);

//! Represents an established connection with a YT cluster.
/*
 *  IConnection instance caches most of the stuff needed for fast interaction
 *  with the cluster (e.g. connection channels, mount info etc).
 *
 *  Thread affinity: any
 */
struct IConnection
    : public virtual TRefCounted
{
    virtual NTabletClient::TTableMountCachePtr GetTableMountCache() = 0;

    virtual IInvokerPtr GetLightInvoker() = 0;
    virtual IInvokerPtr GetHeavyInvoker() = 0;

    virtual IAdminPtr CreateAdmin(const TAdminOptions& options = TAdminOptions()) = 0;
    virtual IClientPtr CreateClient(const TClientOptions& options = TClientOptions()) = 0;

    virtual void ClearMetadataCaches() = 0;
};

DEFINE_REFCOUNTED_TYPE(IConnection)

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

