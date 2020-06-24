#include "block_cache.h"
#include "private.h"
#include "chunk_block_manager.h"

#include <yt/server/node/cluster_node/bootstrap.h>
#include <yt/server/node/cluster_node/config.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/client_block_cache.h>

#include <yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/core/ytalloc/memory_zone.h>

namespace NYT::NDataNode {

using namespace NChunkClient;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

class TServerBlockCache
    : public IBlockCache
{
public:
    TServerBlockCache(
        TDataNodeConfigPtr config,
        NClusterNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , UnderlyingCache_(CreateClientBlockCache(
            Config_->BlockCache,
            EBlockType::UncompressedData,
            DataNodeProfiler.AppendPath("/block_cache")))
    {
        auto result = Bootstrap_->GetMemoryUsageTracker()->TryAcquire(
            EMemoryCategory::BlockCache,
            Config_->BlockCache->GetTotalCapacity());
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error reserving memory for block cache");
    }

    virtual void Put(
        const TBlockId& id,
        EBlockType type,
        const TBlock& data,
        const std::optional<TNodeDescriptor>& source) override
    {
        if (type == EBlockType::CompressedData) {
            const auto& chunkBlockManager = Bootstrap_->GetChunkBlockManager();
            chunkBlockManager->PutCachedBlock(id, data, source);
        } else {
            UnderlyingCache_->Put(id, type, data, source);
        }
    }

    virtual TBlock Find(
        const TBlockId& id,
        EBlockType type) override
    {
        if (type == EBlockType::CompressedData) {
            const auto& chunkBlockManager = Bootstrap_->GetChunkBlockManager();
            if (auto cachedBlock = chunkBlockManager->FindCachedBlock(id)) {
                auto block = cachedBlock->GetData();
                block.BlockOrigin = EBlockOrigin::Cache;
                return block;
            } else {
                return TBlock();
            }
        } else {
            return UnderlyingCache_->Find(id, type);
        }
    }

    virtual EBlockType GetSupportedBlockTypes() const override
    {
        return EBlockType::CompressedData | EBlockType::UncompressedData;
    }

    virtual void OnProfiling() override
    {
        return UnderlyingCache_->OnProfiling();
    }

private:
    const TDataNodeConfigPtr Config_;
    NClusterNode::TBootstrap* const Bootstrap_;

    const IBlockCachePtr UnderlyingCache_;

};

IBlockCachePtr CreateServerBlockCache(
    TDataNodeConfigPtr config,
    NClusterNode::TBootstrap* bootstrap)
{
    return New<TServerBlockCache>(config, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
