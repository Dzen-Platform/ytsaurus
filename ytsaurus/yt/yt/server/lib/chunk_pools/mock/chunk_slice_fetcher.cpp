#include "chunk_slice_fetcher.h"

namespace NYT::NChunkPools {

using namespace NTableClient;
using namespace NPhoenix;

////////////////////////////////////////////////////////////////////////////////

TMockChunkSliceFetcherFactory::TMockChunkSliceFetcherFactory(std::vector<TStrictMockChunkSliceFetcherPtr>* fetchers)
    : Fetchers_(fetchers)
{ }

IChunkSliceFetcherPtr TMockChunkSliceFetcherFactory::CreateChunkSliceFetcher()
{
    YT_VERIFY(CurrentIndex_ < std::ssize(*Fetchers_));
    return Fetchers_->at(CurrentIndex_++);
}

void TMockChunkSliceFetcherFactory::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    // NB: this is a very bad way to persist pointers, but it is ok for unittests.
    if (context.IsSave()) {
        auto fetchersAddress = reinterpret_cast<intptr_t>(Fetchers_);
        Persist(context, fetchersAddress);
    } else {
        intptr_t fetchersAddress;
        Persist(context, fetchersAddress);
        Fetchers_ = reinterpret_cast<std::vector<TStrictMockChunkSliceFetcherPtr>*>(fetchersAddress);
    }
    Persist(context, CurrentIndex_);
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_DYNAMIC_PHOENIX_TYPE(TMockChunkSliceFetcherFactory);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools

