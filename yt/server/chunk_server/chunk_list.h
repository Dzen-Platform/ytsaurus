#pragma once

#include "public.h"
#include "chunk_tree.h"
#include "chunk_tree_statistics.h"

#include <core/misc/property.h>
#include <core/misc/ref_tracked.h>

#include <server/cell_master/public.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct TChunkListDynamicData
    : public NHydra::TEntityDynamicDataBase
{
    //! Used to mark visited chunk lists with "unique" marks.
    ui64 VisitMark = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TChunkList
    : public TChunkTree
    , public TRefTracked<TChunkList>
{
public:
    DEFINE_BYREF_RW_PROPERTY(std::vector<TChunkTree*>, Children);

    // Accumulated sums of children row counts.
    // The i-th value is equal to the sum of row counts of children 0..i
    // for all i in [0..Children.size() - 2]
    // Accumulated statistics for the last child (which is equal to the total chunk list statistics)
    // is stored in #Statistics field.
    DEFINE_BYREF_RW_PROPERTY(std::vector<i64>, RowCountSums);
    // Same as above but for chunk count sums.
    DEFINE_BYREF_RW_PROPERTY(std::vector<i64>, ChunkCountSums);
    // Same as above but for data size sums.
    DEFINE_BYREF_RW_PROPERTY(std::vector<i64>, DataSizeSums);

    DEFINE_BYREF_RW_PROPERTY(yhash_multiset<TChunkList*>, Parents);
    DEFINE_BYREF_RW_PROPERTY(TChunkTreeStatistics, Statistics);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkOwnerBase*>, OwningNodes);

    // Increases each time the list changes.
    // Enables optimistic locking during chunk tree traversing.
    DEFINE_BYVAL_RO_PROPERTY(int, Version);

    ui64 GetVisitMark() const;
    void SetVisitMark(ui64 value);
    static ui64 GenerateVisitMark();

public:
    explicit TChunkList(const TChunkListId& id);

    TChunkListDynamicData* GetDynamicData() const;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    void IncrementVersion();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
