#pragma once

#include "private.h"

#include <yt/server/master/cell_master/public.h>

#include <yt/client/chunk_client/read_limit.h>

#include <yt/core/misc/error.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct IChunkVisitor
    : public virtual TRefCounted
{
    /*!
     *  \note Return |false| to terminate traversing.
     */
    virtual bool OnChunk(
        TChunk* chunk,
        std::optional<i64> rowIndex,
        std::optional<int> tabletIndex,
        const NChunkClient::TLegacyReadLimit& startLimit,
        const NChunkClient::TLegacyReadLimit& endLimit,
        TTransactionId timestampTransactionId) = 0;

    virtual void OnFinish(const TError& error) = 0;

    /*!
     *  \note Return |false| to traverse underlying chunk
     *      or |true| to skip it.
     */
    virtual bool OnChunkView(TChunkView* chunkView) = 0;

    /*!
     *  \note Return |false| to terminate traversing.
     */
    virtual bool OnDynamicStore(
        TDynamicStore* dynamicStore,
        std::optional<int> tabletIndex,
        const NChunkClient::TLegacyReadLimit& startLimit,
        const NChunkClient::TLegacyReadLimit& endLimit) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkVisitor)

////////////////////////////////////////////////////////////////////////////////

struct IChunkTraverserContext
    : public virtual TRefCounted
{
    //! If |true| then #GetInvoker and #GetUnsealedChunkStatistics are not supported;
    //! traverser must run fully synchronously. In particular, not all of its filtering
    //! features are available (e.g. it cannot handle non-trivial bounds).
    virtual bool IsSynchronous() const = 0;

    //! Returns |nullptr| if traversing cannot be preempted.
    virtual IInvokerPtr GetInvoker() const = 0;

    //! Called for each #node pushed onto the stack.
    virtual void OnPop(TChunkTree* node) = 0;

    //! Called for each #node popped from the stack.
    virtual void OnPush(TChunkTree* node) = 0;

    //! Called when traversing finishes; #nodes contains all nodes from the stack.
    virtual void OnShutdown(const std::vector<TChunkTree*>& nodes) = 0;

    //! Called by the traverser to notify the context about the amount of
    //! time spent during traversing.
    virtual void OnTimeSpent(TDuration time) = 0;

    //! See NYT::NJournalClient::TChunkQuorumInfo.
    struct TUnsealedChunkStatistics
    {
        std::optional<i64> FirstOverlayedRowIndex;
        i64 RowCount = 0;
    };

    //! Asynchronously computes the statistics of an unsealed chunk.
    virtual TFuture<TUnsealedChunkStatistics> GetUnsealedChunkStatistics(TChunk* chunk) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkTraverserContext)

////////////////////////////////////////////////////////////////////////////////

IChunkTraverserContextPtr CreateAsyncChunkTraverserContext(
    NCellMaster::TBootstrap* bootstrap,
    NCellMaster::EAutomatonThreadQueue threadQueue);

IChunkTraverserContextPtr GetSyncChunkTraverserContext();

////////////////////////////////////////////////////////////////////////////////

//! Traverses the subtree at #root pruning it to |[lowerLimit, upperLimit)| range.
//! For unsealed chunks, may consult the context to figure out the quorum information.
void TraverseChunkTree(
    IChunkTraverserContextPtr context,
    IChunkVisitorPtr visitor,
    TChunkList* root,
    const NChunkClient::TLegacyReadLimit& lowerLimit,
    const NChunkClient::TLegacyReadLimit& upperLimit,
    std::optional<int> keyColumnCount);

//! Traverses the subtree at #root. No bounds are being checked,
//! #visitor is being notified of each relevant child.
void TraverseChunkTree(
    IChunkTraverserContextPtr context,
    IChunkVisitorPtr visitor,
    TChunkList* root);

//! Appends the chunks found in subtree at #root to #chunks.
void EnumerateChunksInChunkTree(
    TChunkList* root,
    std::vector<TChunk*>* chunks);

//! Returns the list of all chunks in subtree at #root.
std::vector<TChunk*> EnumerateChunksInChunkTree(
    TChunkList* root);

//! Appends the chunks, chunk views, and dynamic stores found in subtree at #root to #stores.
void EnumerateStoresInChunkTree(
    TChunkList* root,
    std::vector<TChunkTree*>* stores);

//! Returns the list of chunks, chunk views, and dynamic stores found in subtree at #root.
std::vector<TChunkTree*> EnumerateStoresInChunkTree(TChunkList* root);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
