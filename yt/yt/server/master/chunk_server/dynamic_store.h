#pragma once

#include "chunk_tree.h"
#include "chunk_tree_statistics.h"
#include "public.h"

#include <yt/server/master/cell_master/public.h>

#include <yt/server/master/tablet_server/public.h>

#include <yt/core/misc/ref_tracked.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TDynamicStore
    : public TChunkTree
    , public TRefTracked<TDynamicStore>
{
public:
    using TParents = SmallVector<TChunkList*, TypicalChunkParentCount>;

    DEFINE_BYVAL_RW_PROPERTY(const NTabletServer::TTablet*, Tablet);
    DEFINE_BYVAL_RO_PROPERTY(TChunk*, FlushedChunk);
    DEFINE_BYREF_RO_PROPERTY(TParents, Parents);
    //! Used for flushed ordered dynamic stores. Denotes the (tablet-wise) row index
    //! of the first row in the chunk.
    DEFINE_BYVAL_RW_PROPERTY(i64, TableRowIndex);

public:
    explicit TDynamicStore(TDynamicStoreId id);

    virtual TString GetLowercaseObjectName() const override;
    virtual TString GetCapitalizedObjectName() const override;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    // May be nullptr in case if no rows were flushed.
    void SetFlushedChunk(TChunk* chunk);
    bool IsFlushed() const;

    // Dynamic store is abandoned if it was removed without flush if
    // the tablet was forcefully removed or experienced overwrite bulk insert.
    void Abandon();
    bool IsAbandoned() const;

    void AddParent(TChunkList* parent);
    void RemoveParent(TChunkList* parent);

    TChunkTreeStatistics GetStatistics() const;

private:
    bool Flushed_ = false;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
