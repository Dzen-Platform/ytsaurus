#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/ytlib/chunk_client/file_reader.h>

#include <yt/core/actions/signal.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/rw_spinlock.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/property.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

//! Manages stored chunks.
/*!
 *  \note
 *  Thread affinity: ControlThread (unless indicated otherwise)
 */
class TChunkStore
    : public TRefCounted
{
public:
    typedef std::vector<IChunkPtr> TChunks;
    typedef std::vector<TStoreLocationPtr> TLocations;

    TChunkStore(
        TDataNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

    void Initialize();

    //! Registers a just-written chunk.
    void RegisterNewChunk(IChunkPtr chunk);

    //! Registers a chunk found during startup.
    void RegisterExistingChunk(IChunkPtr chunk);

    //! Triggers another round of master notification for a chunk that is already registered.
    /*!
     *  Used for journal chunks that initially get registered (with "active" replica type)
     *  when a session starts and subsequently get re-registered (with "unsealed" replica type)
     *  with the session finishes. Finally, when such a chunk is sealed it gets re-registered again
     *  (with "sealed" replica type).
     */
    void UpdateExistingChunk(IChunkPtr chunk);

    //! Unregisters the chunk but does not remove any of its files.
    void UnregisterChunk(IChunkPtr chunk);

    //! Finds chunk by id. Returns |nullptr| if no chunk exists.
    /*!
     *  \note
     *  Thread affinity: any
     */
    IChunkPtr FindChunk(const TChunkId& chunkId) const;

    //! Finds chunk by id. Throws if no chunk exists.
    /*!
     *  \note
     *  Thread affinity: any
     */
    IChunkPtr GetChunkOrThrow(const TChunkId& chunkId) const;

    //! Returns the list of all registered chunks.
    /*!
     *  \note
     *  Thread affinity: any
     */
    TChunks GetChunks() const;

    //! Returns the number of registered chunks.
    /*!
     *  \note
     *  Thread affinity: any
     */
    int GetChunkCount() const;

    //! Physically removes the chunk.
    /*!
     *  This call also evicts the reader from the cache thus hopefully closing the file.
     */
    TFuture<void> RemoveChunk(IChunkPtr chunk);

    //! Finds a suitable storage location for a new chunk.
    /*!
     *  Among enabled locations that are not full and support chunks of a given type,
     *  returns a random one with the minimum number of active sessions.
     *
     *  Throws exception if no suitable location could be found.
     */
    TStoreLocationPtr GetNewChunkLocation(NObjectClient::EObjectType chunkType);

    //! Storage locations.
    DEFINE_BYREF_RO_PROPERTY(TLocations, Locations);

    //! Raised when a chunk is added to the store.
    DEFINE_SIGNAL(void(IChunkPtr), ChunkAdded);

    //! Raised when a chunk is removed from the store.
    DEFINE_SIGNAL(void(IChunkPtr), ChunkRemoved);

private:
    TDataNodeConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;

    struct TChunkEntry
    {
        IChunkPtr Chunk;
        i64 DiskSpace = 0;
    };

    NConcurrency::TReaderWriterSpinLock ChunkMapLock_;
    yhash_map<TChunkId, TChunkEntry> ChunkMap_;


    void DoRegisterChunk(const TChunkEntry& entry);

    static TChunkEntry BuildEntry(IChunkPtr chunk);
    IChunkPtr CreateFromDescriptor(TStoreLocationPtr location, const TChunkDescriptor& descriptor);

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

};

DEFINE_REFCOUNTED_TYPE(TChunkStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

