#include "ytree_integration.h"
#include "artifact.h"
#include "chunk.h"
#include "chunk_cache.h"
#include "chunk_store.h"
#include "location.h"

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/virtual.h>

namespace NYT {
namespace NDataNode {

using namespace NYTree;
using namespace NYson;
using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

template <class TCollection>
class TVirtualChunkMap
    : public TVirtualMapBase
{
public:
    explicit TVirtualChunkMap(TIntrusivePtr<TCollection> collection)
        : Collection(collection)
    { }

private:
    TIntrusivePtr<TCollection> Collection;

    virtual std::vector<TString> GetKeys(i64 sizeLimit) const override
    {
        auto chunks = Collection->GetChunks();
        std::vector<TString> keys;
        keys.reserve(chunks.size());
        for (auto chunk : chunks) {
            keys.push_back(ToString(chunk->GetId()));
            if (keys.size() == sizeLimit)
                break;
        }
        return keys;
    }

    virtual i64 GetSize() const override
    {
        return Collection->GetChunkCount();
    }

    virtual IYPathServicePtr FindItemService(const TStringBuf& key) const override
    {
        auto id = TChunkId::FromString(key);
        auto chunk = Collection->FindChunk(id);
        if (!chunk) {
            return nullptr;
        }

        return IYPathService::FromProducer(BIND([=] (IYsonConsumer* consumer) {
            BuildYsonFluently(consumer)
                .BeginMap()
                    .Item("disk_space").Value(chunk->GetInfo().disk_space())
                    .Item("location").Value(chunk->GetLocation()->GetPath())
                    .Item("artifact").Value(IsArtifactChunkId(chunk->GetId()))
                .EndMap();
        }));
    }

};

IYPathServicePtr CreateStoredChunkMapService(TChunkStorePtr chunkStore)
{
    return New< TVirtualChunkMap<TChunkStore> >(chunkStore);
}

IYPathServicePtr CreateCachedChunkMapService(TChunkCachePtr chunkCache)
{
    return New< TVirtualChunkMap<TChunkCache> >(chunkCache);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
