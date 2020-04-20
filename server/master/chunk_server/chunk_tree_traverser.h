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
        i64 rowIndex,
        std::optional<i32> tabletIndex,
        const NChunkClient::TReadLimit& startLimit,
        const NChunkClient::TReadLimit& endLimit,
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
        const NChunkClient::TReadLimit& startLimit,
        const NChunkClient::TReadLimit& endLimit) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkVisitor)

////////////////////////////////////////////////////////////////////////////////

struct IChunkTraverserCallbacks
    : public virtual TRefCounted
{
    //! Returns |nullptr| if traversing cannot be preempted.
    virtual IInvokerPtr GetInvoker() const = 0;

    //! Called for each #node pushed onto the stack.
    virtual void OnPop(TChunkTree* node) = 0;

    //! Called for each #node popped from the stack.
    virtual void OnPush(TChunkTree* node) = 0;

    //! Called when traversing finishes; #nodes contains all nodes from the stack.
    virtual void OnShutdown(const std::vector<TChunkTree*>& nodes) = 0;

    //! Called by the traverser to notify the callbacks about the amount of
    //! time spent during traversing.
    virtual void OnTimeSpent(TDuration time) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkTraverserCallbacks)

////////////////////////////////////////////////////////////////////////////////

IChunkTraverserCallbacksPtr CreatePreemptableChunkTraverserCallbacks(
    NCellMaster::TBootstrap* bootstrap,
    NCellMaster::EAutomatonThreadQueue threadQueue);

IChunkTraverserCallbacksPtr GetNonpreemptableChunkTraverserCallbacks();

void TraverseChunkTree(
    IChunkTraverserCallbacksPtr callbacks,
    IChunkVisitorPtr visitor,
    TChunkList* root,
    const NChunkClient::TReadLimit& lowerLimit = NChunkClient::TReadLimit(),
    const NChunkClient::TReadLimit& upperLimit = NChunkClient::TReadLimit());

void EnumerateChunksInChunkTree(
    TChunkList* root,
    std::vector<TChunk*>* chunks,
    const NChunkClient::TReadLimit& lowerBound = NChunkClient::TReadLimit(),
    const NChunkClient::TReadLimit& upperBound = NChunkClient::TReadLimit());

std::vector<TChunk*> EnumerateChunksInChunkTree(
    TChunkList* root,
    const NChunkClient::TReadLimit& lowerBound = NChunkClient::TReadLimit(),
    const NChunkClient::TReadLimit& upperBound = NChunkClient::TReadLimit());

// Enumerates chunks, chunk views and dynamic stores.
void EnumerateStoresInChunkTree(TChunkList* root, std::vector<TChunkTree*>* stores);

std::vector<TChunkTree*> EnumerateStoresInChunkTree(TChunkList* root);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
