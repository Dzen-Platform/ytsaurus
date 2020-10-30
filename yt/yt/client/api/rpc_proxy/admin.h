#pragma once

#include "public.h"

#include <yt/client/api/admin.h>

#include <yt/client/hydra/public.h>

#include <yt/core/concurrency/public.h>

namespace NYT::NApi::NRpcProxy {

///////////////////////////////////////////////////////////////////////////////

class TAdmin
    : public NApi::IAdmin
{
public:
    explicit TAdmin(NRpc::IChannelPtr channel);

    virtual TFuture<int> BuildSnapshot(
        const NApi::TBuildSnapshotOptions& options = {}) override;

    virtual TFuture<TCellIdToSnapshotIdMap> BuildMasterSnapshots(
        const TBuildMasterSnapshotsOptions& options = {}) override;

    virtual TFuture<void> SwitchLeader(
        NHydra::TCellId cellId,
        NHydra::TPeerId newLeaderId,
        const TSwitchLeaderOptions& options = {}) override;

    virtual TFuture<void> GCCollect(
        const NApi::TGCCollectOptions& options = {}) override;

    virtual TFuture<void> KillProcess(
        const TString& address,
        const NApi::TKillProcessOptions& options = {}) override;

    virtual TFuture<TString> WriteCoreDump(
        const TString& address,
        const NApi::TWriteCoreDumpOptions& options = {}) override;

    virtual TFuture<TString> WriteOperationControllerCoreDump(
        NJobTrackerClient::TOperationId operationId,
        const NApi::TWriteOperationControllerCoreDumpOptions& options = {}) override;

private:
    const NRpc::IChannelPtr Channel_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
