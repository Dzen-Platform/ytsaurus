#pragma once

#include "public.h"

#include <core/misc/ref.h>

#include <core/actions/future.h>

#include <server/hydra/public.h>

#include <server/cell_node/public.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

//! Manages journal chunks stored at some specific location.
class TJournalManager
    : public TRefCounted
{
public:
    TJournalManager(
        TDataNodeConfigPtr config,
        TStoreLocation* location,
        NCellNode::TBootstrap* bootstrap);
    ~TJournalManager();

    void Initialize();

    TFuture<NHydra::IChangelogPtr> OpenChangelog(
        const TChunkId& chunkId);

    TFuture<NHydra::IChangelogPtr> CreateChangelog(
        const TChunkId& chunkId,
        bool enableMultiplexing);

    TFuture<void> RemoveChangelog(
        TJournalChunkPtr chunk,
        bool enableMultiplexing);

    TFuture<void> AppendMultiplexedRecord(
        const TChunkId& chunkId,
        int recordId,
        const TSharedRef& recordData,
        TFuture<void> splitResult);

    TFuture<bool> IsChangelogSealed(const TChunkId& chunkId);

    TFuture<void> SealChangelog(TJournalChunkPtr chunk);

private:
    class TImpl;
    typedef TIntrusivePtr<TImpl> TImplPtr;

    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TJournalManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

