#pragma once

#include "store_manager_detail.h"
#include "dynamic_store_bits.h"

#include <yt/server/node/cluster_node/public.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/tablet_client/public.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TOrderedStoreManager
    : public TStoreManagerBase
    , public IOrderedStoreManager
{
public:
    TOrderedStoreManager(
        TTabletManagerConfigPtr config,
        TTablet* tablet,
        ITabletContext* tabletContext,
        NHydra::IHydraManagerPtr hydraManager = nullptr,
        IInMemoryManagerPtr inMemoryManager = nullptr,
        NApi::NNative::IClientPtr client = nullptr);

    // IStoreManager overrides.
    virtual void Mount(
        const std::vector<NTabletNode::NProto::TAddStoreDescriptor>& storeDescriptors,
        bool createDynamicStore,
        const NTabletNode::NProto::TMountHint& mountHint) override;

    virtual bool ExecuteWrites(
        NTableClient::TWireProtocolReader* reader,
        TWriteContext* context) override;

    TOrderedDynamicRowRef WriteRow(
        TUnversionedRow row,
        TWriteContext* context);

    virtual bool IsFlushNeeded() const override;
    virtual bool IsStoreCompactable(IStorePtr store) const override;
    virtual bool IsStoreFlushable(IStorePtr store) const override;

    virtual IOrderedStoreManagerPtr AsOrdered() override;

private:
    TOrderedDynamicStorePtr ActiveStore_;

    virtual IDynamicStore* GetActiveStore() const override;
    virtual void ResetActiveStore() override;
    virtual void OnActiveStoreRotated() override;

    virtual TStoreFlushCallback MakeStoreFlushCallback(
        IDynamicStorePtr store,
        TTabletSnapshotPtr tabletSnapshot,
        bool isUnmountWorkflow) override;

    i64 ComputeStartingRowIndex() const;
    virtual void CreateActiveStore() override;
};

DEFINE_REFCOUNTED_TYPE(TOrderedStoreManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
