#include "chunk_block_manager.h"

#include "bootstrap.h"
#include "private.h"
#include "blob_reader_cache.h"
#include "chunk.h"
#include "chunk_registry.h"
#include "config.h"
#include "location.h"
#include "p2p.h"

#include <yt/yt/server/node/cluster_node/config.h>

#include <yt/yt/server/lib/io/chunk_file_reader.h>

#include <yt/yt/ytlib/chunk_client/block_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/data_node_service_proxy.h>

#include <yt/yt_proto/yt/client/chunk_client/proto/chunk_meta.pb.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/concurrency/thread_affinity.h>
#include <yt/yt/core/concurrency/thread_pool.h>

#include <yt/yt/core/ytalloc/memory_zone.h>

namespace NYT::NDataNode {

using namespace NObjectClient;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NClusterNode;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TChunkBlockManager
    : public IChunkBlockManager
{
public:
    explicit TChunkBlockManager(IBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    TFuture<std::vector<TBlock>> ReadBlockRange(
        TChunkId chunkId,
        int firstBlockIndex,
        int blockCount,
        const TChunkReadOptions& options) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        try {
            const auto& chunkRegistry = Bootstrap_->GetChunkRegistry();
            // NB: At the moment, range read requests are only possible for the whole chunks.
            auto chunk = chunkRegistry->GetChunkOrThrow(chunkId);
            return chunk->ReadBlockRange(
                firstBlockIndex,
                blockCount,
                options);
        } catch (const std::exception& ex) {
            return MakeFuture<std::vector<TBlock>>(TError(ex));
        }
    }

    TFuture<std::vector<TBlock>> ReadBlockSet(
        TChunkId chunkId,
        const std::vector<int>& blockIndexes,
        const TChunkReadOptions& options) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        try {
            const auto& chunkRegistry = Bootstrap_->GetChunkRegistry();
            auto chunk = chunkRegistry->FindChunk(chunkId);
            if (!chunk) {
                return MakeFuture<std::vector<TBlock>>({});
            }
            return chunk->ReadBlockSet(blockIndexes, options);
        } catch (const std::exception& ex) {
            return MakeFuture<std::vector<TBlock>>(TError(ex));
        }
    }

private:
    IBootstrap* const Bootstrap_;
};

////////////////////////////////////////////////////////////////////////////////

IChunkBlockManagerPtr CreateChunkBlockManager(IBootstrap* bootstrap)
{
    return New<TChunkBlockManager>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
