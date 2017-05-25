#pragma once

#include "public.h"
#include "connection.h"

#include <yt/ytlib/tablet_client/public.h>

#include <yt/core/actions/future.h>

namespace NYT {
namespace NApi {

////////////////////////////////////////////////////////////////////////////////

struct TBuildSnapshotOptions
{
    //! Refers either to masters or to tablet cells.
    //! If null then the primary one is assumed.
    NElection::TCellId CellId;
    bool SetReadOnly = false;
};

struct TGCCollectOptions
{
    //! Refers to master cell.
    //! If null then the primary one is assumed.
    NElection::TCellId CellId;
};

struct TKillProcessOptions
{
    int ExitCode = 42;
};

struct TWriteCoreDumpOptions
{ };

struct IAdmin
    : public virtual TRefCounted
{
    virtual TFuture<int> BuildSnapshot(
        const TBuildSnapshotOptions& options = TBuildSnapshotOptions()) = 0;

    virtual TFuture<void> GCCollect(
        const TGCCollectOptions& options = TGCCollectOptions()) = 0;

    virtual TFuture<void> KillProcess(
        const Stroka& address,
        const TKillProcessOptions& options = TKillProcessOptions()) = 0;

    virtual TFuture<Stroka> WriteCoreDump(
        const Stroka& address,
        const TWriteCoreDumpOptions& options = TWriteCoreDumpOptions()) = 0;
};

DEFINE_REFCOUNTED_TYPE(IAdmin)

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

