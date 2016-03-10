#include "store.h"
#include "sorted_chunk_store.h"
#include "sorted_dynamic_store.h"

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

bool IStore::IsDynamic() const
{
    auto type = GetType();
    return type == EStoreType::SortedDynamic;
}

IDynamicStorePtr IStore::AsDynamic()
{
    auto* result = dynamic_cast<IDynamicStore*>(this);
    YCHECK(result);
    return result;
}

bool IStore::IsChunk() const
{
    auto type = GetType();
    return type == EStoreType::SortedChunk;
}

IChunkStorePtr IStore::AsChunk()
{
    auto* result = dynamic_cast<IChunkStore*>(this);
    YCHECK(result);
    return result;
}

bool IStore::IsSorted() const
{
    auto type = GetType();
    return type == EStoreType::SortedDynamic ||
           type == EStoreType::SortedChunk;
}

ISortedStorePtr IStore::AsSorted()
{
    auto* result = dynamic_cast<ISortedStore*>(this);
    YCHECK(result);
    return result;
}

TSortedDynamicStorePtr IStore::AsSortedDynamic()
{
    auto* result = dynamic_cast<TSortedDynamicStore*>(this);
    YCHECK(result);
    return result;
}

TSortedChunkStorePtr IStore::AsSortedChunk()
{
    auto* result = dynamic_cast<TSortedChunkStore*>(this);
    YCHECK(result);
    return result;
}

bool IStore::IsOrdered() const
{
    // TODO(babenko): ordered stores
    return false;
}

IOrderedStorePtr IStore::AsOrdered()
{
    auto* result = dynamic_cast<IOrderedStore*>(this);
    YCHECK(result);
    return result;
}

////////////////////////////////////////////////////////////////////////////////

void TStoreIdFormatter::operator()(TStringBuilder* builder, const IStorePtr& store) const
{
    builder->AppendFormat("%v", store->GetId());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
