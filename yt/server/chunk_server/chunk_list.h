#pragma once

#include "public.h"
#include "chunk_tree.h"
#include "chunk_tree_statistics.h"

#include <yt/server/cell_master/public.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct TChunkListDynamicData
    : public NObjectServer::TObjectDynamicData
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
    //! This many starting children are null.
    DEFINE_BYVAL_RW_PROPERTY(int, TrimmedChildCount);
    DEFINE_BYREF_RW_PROPERTY(std::vector<TChunkTree*>, Children);

    //! If |true|, then child-to-index map is maintained but no sums are accumulated.
    //! If |false|, then vice verse, sums are accumulated but no child-to-index map exists.
    DEFINE_BYVAL_RO_PROPERTY(bool, Ordered);

    using TChildToIndexMap = yhash_map<TChunkTree*, int>;
    DEFINE_BYREF_RW_PROPERTY(TChildToIndexMap, ChildToIndex);

    struct TCumulativeStatisticsEntry
    {
        i64 RowCount;
        i64 ChunkCount;
        i64 DataSize;

        void Persist(NCellMaster::TPersistenceContext& context);
    };

    // The i-th value is equal to the sum of statistics for children 0..i
    // for all i in [0..Children.size() - 2]
    // NB: Cumulative statistics for the last child (which is equal to the total chunk list statistics)
    // is stored in #Statistics field.
    DEFINE_BYREF_RW_PROPERTY(std::vector<TCumulativeStatisticsEntry>, CumulativeStatistics);

    DEFINE_BYREF_RW_PROPERTY(TChunkTreeStatistics, Statistics);

    // These lists is typically small, e.g. has the length of 1.
    // However it may become pretty large in some real-world scenarios.
    // Hence we maintain a separate item-to-index maps to speedup deletions.
    DEFINE_BYREF_RO_PROPERTY(std::vector<TChunkList*>, Parents);
    DEFINE_BYREF_RO_PROPERTY(std::vector<TChunkOwnerBase*>, OwningNodes);

    // Increases each time the list changes.
    // Enables optimistic locking during chunk tree traversing.
    DEFINE_BYVAL_RO_PROPERTY(int, Version);

public:
    explicit TChunkList(const TChunkListId& id);

    TChunkListDynamicData* GetDynamicData() const;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    void AddParent(TChunkList* parent);
    void RemoveParent(TChunkList* parent);

    void AddOwningNode(TChunkOwnerBase* node);
    void RemoveOwningNode(TChunkOwnerBase* node);

    void IncrementVersion();

    void ValidateSealed();

    ui64 GetVisitMark() const;
    void SetVisitMark(ui64 value);
    static ui64 GenerateVisitMark();

    virtual int GetGCWeight() const override;

    void SetOrdered(bool value);

private:
    yhash_map<TChunkList*, int> ParentToIndex_;
    yhash_map<TChunkOwnerBase*, int> OwningNodeToIndex_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT


#define CHUNK_LIST_INL_H_
#include "chunk_list-inl.h"
#undef CHUNK_LIST_INL_H_
