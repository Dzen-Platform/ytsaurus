#include "client_block_cache.h"
#include "private.h"
#include "block_cache.h"
#include "config.h"
#include "block_id.h"

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/config.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/singleton.h>
#include <yt/core/misc/sync_cache.h>

namespace NYT::NChunkClient {

using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkClientLogger;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TCachedBlock)

class TCachedBlock
    : public TSyncCacheValueBase<TBlockId, TCachedBlock>
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TBlock, Data);

public:
    TCachedBlock(const TBlockId& id, const TBlock& data)
        : TSyncCacheValueBase(id)
        , Data_(data)
    { }

};

DEFINE_REFCOUNTED_TYPE(TCachedBlock)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TPerTypeClientBlockCache)

class TPerTypeClientBlockCache
    : public TSyncSlruCacheBase<TBlockId, TCachedBlock>
{
public:
    TPerTypeClientBlockCache(
        EBlockType type,
        TSlruCacheConfigPtr config,
        const NProfiling::TProfiler& profiler)
        : TSyncSlruCacheBase(config, profiler)
        , Type_(type)
    { }

    void Put(const TBlockId& id, const TBlock& data)
    {
        if (Config_->Capacity == 0) {
            // Shortcut when cache is disabled.
            return;
        }

        auto block = New<TCachedBlock>(id, data);
        if (TryInsert(block)) {
            YT_LOG_DEBUG("Block is put into cache (BlockId: %v, BlockType: %v, BlockSize: %v)",
                id,
                Type_,
                data.Size());
        } else {
            // Already have the block cached, do nothing.
            YT_LOG_TRACE("Block is already in cache (BlockId: %v, BlockType: %v)",
                id,
                Type_);
        }
    }

    TBlock Find(const TBlockId& id)
    {
        if (Config_->Capacity == 0) {
            // Shortcut when cache is disabled.
            return TBlock();
        }

        auto block = TSyncSlruCacheBase::Find(id);
        if (block) {
            YT_LOG_TRACE("Block cache hit (BlockId: %v, BlockType: %v)",
                id,
                Type_);
            return block->GetData();
        } else {
            YT_LOG_TRACE("Block cache miss (BlockId: %v, BlockType: %v)",
                id,
                Type_);
            return TBlock();
        }
    }

    void OnProfiling()
    {
        TSyncSlruCacheBase::OnProfiling();
    }

private:
    const EBlockType Type_;

    virtual i64 GetWeight(const TCachedBlockPtr& block) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return block->GetData().Size();
    }

};

DEFINE_REFCOUNTED_TYPE(TPerTypeClientBlockCache)

////////////////////////////////////////////////////////////////////////////////

class TClientBlockCache
    : public IBlockCache
{
public:
    TClientBlockCache(
        TBlockCacheConfigPtr config,
        EBlockType supportedBlockTypes,
        const NProfiling::TProfiler& profiler)
        : SupportedBlockTypes_(supportedBlockTypes)
    {
        auto initType = [&] (EBlockType type, TSlruCacheConfigPtr config) {
            if (Any(SupportedBlockTypes_ & type)) {
                auto cache = New<TPerTypeClientBlockCache>(
                    type,
                    config,
                    profiler.AppendPath("/" + FormatEnum(type)));
                YT_VERIFY(PerTypeCaches_.insert({type, cache}).second);
            }
        };
        initType(EBlockType::CompressedData, config->CompressedData);
        initType(EBlockType::UncompressedData, config->UncompressedData);
    }

    virtual void Put(
        const TBlockId& id,
        EBlockType type,
        const TBlock& data,
        const std::optional<TNodeDescriptor>& /*source*/) override
    {
        auto cache = FindPerTypeCache(type);
        if (cache) {
            cache->Put(id, data);
        }
    }

    virtual TBlock Find(
        const TBlockId& id,
        EBlockType type) override
    {
        auto cache = FindPerTypeCache(type);
        return cache ? cache->Find(id) : TBlock();
    }

    virtual EBlockType GetSupportedBlockTypes() const override
    {
        return SupportedBlockTypes_;
    }

    virtual void OnProfiling()
    {
        for (const auto& [_, cache] : PerTypeCaches_) {
            cache->OnProfiling();
        }
    }

private:
    const EBlockType SupportedBlockTypes_;

    THashMap<EBlockType, TPerTypeClientBlockCachePtr> PerTypeCaches_;

    TPerTypeClientBlockCachePtr FindPerTypeCache(EBlockType type)
    {
        auto it = PerTypeCaches_.find(type);
        return it == PerTypeCaches_.end() ? nullptr : it->second;
    }

};

IBlockCachePtr CreateClientBlockCache(
    TBlockCacheConfigPtr config,
    EBlockType supportedBlockTypes,
    const NProfiling::TProfiler& profiler)
{
    return New<TClientBlockCache>(config, supportedBlockTypes, profiler);
}

////////////////////////////////////////////////////////////////////////////////

class TNullBlockCache
    : public IBlockCache
{
public:
    virtual void Put(
        const TBlockId& /*id*/,
        EBlockType /*type*/,
        const TBlock& /*data*/,
        const std::optional<TNodeDescriptor>& /*source*/) override
    { }

    virtual TBlock Find(
        const TBlockId& /*id*/,
        EBlockType /*type*/) override
    {
        return TBlock();
    }

    virtual EBlockType GetSupportedBlockTypes() const override
    {
        return EBlockType::None;
    }

    virtual void OnProfiling() override
    { }
};

IBlockCachePtr GetNullBlockCache()
{
    return RefCountedSingleton<TNullBlockCache>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient

