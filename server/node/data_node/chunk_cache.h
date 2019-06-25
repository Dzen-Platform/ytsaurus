#pragma once

#include "public.h"

#include <yt/server/node/cell_node/public.h>

#include <yt/client/chunk_client/chunk_replica.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/actions/signal.h>

#include <yt/core/misc/error.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

struct TArtifactDownloadOptions
{
    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory;
    NChunkClient::TTrafficMeterPtr TrafficMeter;
};

//! Manages chunks cached at Data Node.
/*!
 *  \note
 *  Thread affinity: ControlThread (unless indicated otherwise)
 */
class TChunkCache
    : public TRefCounted
{
public:
    TChunkCache(
        TDataNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap);
    ~TChunkCache();

    void Initialize();

    bool IsEnabled() const;

    //! Finds chunk by id. Returns |nullptr| if no chunk exists.
    /*!
     *  \note
     *  Thread affinity: any
     */
    IChunkPtr FindChunk(TChunkId chunkId);

    //! Returns the list of all registered chunks.
    /*!
     *  \note
     *  Thread affinity: any
     */
    std::vector<IChunkPtr> GetChunks();

    //! Returns the number of registered chunks.
    /*!
     *  \note
     *  Thread affinity: any
     */
    int GetChunkCount();

    //! Downloads a single- or multi-chunk artifact into the cache.
    /*!
     *  The download process is asynchronous.
     *  If the chunk is already cached, it returns a pre-set result.
     *
     *  Thread affinity: any
     */
    TFuture<IChunkPtr> DownloadArtifact(
        const TArtifactKey& key,
        const TArtifactDownloadOptions& options);

    //! Constructs a producer that will download the artifact and feed its content to a stream.
    /*!
     *  Thread affinity: any
     */
    std::function<void(IOutputStream*)> MakeArtifactDownloadProducer(
        const TArtifactKey& key,
        const TArtifactDownloadOptions& options);

    //! Cache locations.
    DECLARE_BYREF_RO_PROPERTY(std::vector<TCacheLocationPtr>, Locations);

    //! Raised when a chunk is added to the cache.
    DECLARE_SIGNAL(void(IChunkPtr), ChunkAdded);

    //! Raised when a chunk is removed from the cache.
    DECLARE_SIGNAL(void(IChunkPtr), ChunkRemoved);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TChunkCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode

