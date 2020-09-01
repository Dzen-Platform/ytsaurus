#pragma once

#include "public.h"

#include <yt/client/object_client/public.h>

#include <yt/core/actions/public.h>

#include <yt/core/misc/ref_counted.h>

#include <yt/core/rpc/public.h>

namespace NYT::NCellMaster {

///////////////////////////////////////////////////////////////////////////////

//! A helper for syncing with other master cells in multiple phases.
/*!
 *  Stores the set of already synced-with cells, thus avoiding syncing with the
 *  same cell twice.
 *
 *  For use in those situations when, after one sync is done, there may arise a
 *  need to sync with some additional cells (and so on).
 */
class TMultiPhaseCellSyncSession
    : public TRefCounted
{
public:
    TMultiPhaseCellSyncSession(
        TBootstrap* bootstrap,
        bool syncWithUpstream,
        NRpc::TRequestId requestId); // For logging purposes only.

    // NB: the #additionalFutures is just to save some allocations and avoid doing this all the time:
    //   auto syncFuture = session->Sync(); // Already calls #AllSucceeded internally.
    //   additionalFutures.push_back(std::move(syncFuture));
    //   AllSucceeded(std::move(additionalFutures)); // Second call to #AllSucceeded.
    TFuture<void> Sync(const NObjectClient::TCellTagList& cellTags, std::vector<TFuture<void>> additionalFutures = {});
    TFuture<void> Sync(const NObjectClient::TCellTagList& cellTags, TFuture<void> additionalFuture);

private:
    TBootstrap* const Bootstrap_;
    const bool SyncWithUpstream_;
    const NRpc::TRequestId RequestId_;
    int PhaseNumber_ = 0;
    NObjectClient::TCellTagList SyncedWithCellTags_;

    bool RegisterCellToSyncWith(NObjectClient::TCellTag cellTag);
};

DEFINE_REFCOUNTED_TYPE(TMultiPhaseCellSyncSession);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
