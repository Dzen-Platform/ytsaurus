#pragma once

#include "public.h"

#include <yt/yt/server/lib/hydra_common/mutation.h>

#include <yt/yt/ytlib/election/public.h>

#include <yt/yt/core/rpc/public.h>

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TWorldInitializer
    : public TRefCounted
{
public:
    TWorldInitializer(
        TCellMasterConfigPtr config,
        TBootstrap* bootstrap);
    ~TWorldInitializer();

    //! Returns |true| if the cluster is initialized.
    bool IsInitialized();

    //! Checks if the cluster is initialized. Throws if not.
    void ValidateInitialized();

    //! Returns |true| if provision lock is active.
    //! May only be called on the primary cell.
    bool HasProvisionLock();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TWorldInitializer)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
