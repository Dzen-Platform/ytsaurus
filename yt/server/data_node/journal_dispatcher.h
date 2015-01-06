#pragma once

#include "public.h"

#include <core/misc/error.h>

#include <server/hydra/public.h>

#include <server/cell_node/public.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

//! Manages cached journals.
class TJournalDispatcher
    : public TRefCounted
{
public:
    TJournalDispatcher(
        NCellNode::TBootstrap* bootstrap,
        TDataNodeConfigPtr config);
    ~TJournalDispatcher();

    void Initialize();

    //! Returns |true| if new journal chunks are accepted.
    bool AcceptsChunks() const;

    //! Asynchronously opens (or returns a cached) changelog corresponding
    //! to a given journal chunk.
    TFuture<NHydra::IChangelogPtr> OpenChangelog(
        TLocationPtr location,
        const TChunkId& chunkId,
        bool enableMultiplexing);

    //! Asynchronously creates a new changelog corresponding to a given journal chunk.
    TFuture<NHydra::IChangelogPtr> CreateChangelog(
        TJournalChunkPtr chunk,
        bool enableMultiplexing);

    //! Asynchronously removes files of a given journal chunk.
    TFuture<void> RemoveChangelog(TJournalChunkPtr chunk);

private:
    class TCachedChangelog;
    typedef TIntrusivePtr<TCachedChangelog> TCachedChangelogPtr;

    class TImpl;
    typedef TIntrusivePtr<TImpl> TImplPtr;

    class TMultiplexedReplay;

    TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TJournalDispatcher)

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

