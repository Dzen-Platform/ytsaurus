#pragma once

#include "public.h"
#include "chunk_tree.h"
#include "chunk_tree_statistics.h"

#include <yt/server/cell_master/public.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>
#include <yt/core/misc/indexed_vector.h>
#include <yt/core/misc/range.h>

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

    DEFINE_BYREF_RW_PROPERTY(TChunkTreeStatistics, Statistics);

    // Increases each time the list changes.
    // Enables optimistic locking during chunk tree traversing.
    DEFINE_BYVAL_RO_PROPERTY(int, Version);

public:
    explicit TChunkList(const TChunkListId& id);

    TChunkListDynamicData* GetDynamicData() const;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    TRange<TChunkList*> Parents() const;
    void AddParent(TChunkList* parent);
    void RemoveParent(TChunkList* parent);

    TRange<TChunkOwnerBase*> TrunkOwningNodes() const;
    TRange<TChunkOwnerBase*> BranchedOwningNodes() const;
    void AddOwningNode(TChunkOwnerBase* node);
    void RemoveOwningNode(TChunkOwnerBase* node);

    void IncrementVersion();

    void ValidateSealed();

    ui64 GetVisitMark() const;
    void SetVisitMark(ui64 value);
    static ui64 GenerateVisitMark();

    virtual int GetGCWeight() const override;

private:
    TIndexedVector<TChunkList*> Parents_;
    TIndexedVector<TChunkOwnerBase*> TrunkOwningNodes_;
    TIndexedVector<TChunkOwnerBase*> BranchedOwningNodes_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT


#define CHUNK_LIST_INL_H_
#include "chunk_list-inl.h"
#undef CHUNK_LIST_INL_H_
