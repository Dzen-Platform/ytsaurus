#include "chunk_visitor.h"

namespace NYT::NChunkServer {

using namespace NChunkClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TChunkVisitorBase::TChunkVisitorBase(
    NCellMaster::TBootstrap* bootstrap,
    TChunkList* chunkList)
    : Bootstrap_(bootstrap)
    , ChunkList_(chunkList)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
}

TFuture<TYsonString> TChunkVisitorBase::Run()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto context = CreateAsyncChunkTraverserContext(
        Bootstrap_,
        NCellMaster::EAutomatonThreadQueue::ChunkStatisticsTraverser);
    TraverseChunkTree(
        std::move(context),
        this,
        ChunkList_);

    return Promise_;
}

void TChunkVisitorBase::OnFinish(const TError& error)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (error.IsOK()) {
        OnSuccess();
    } else {
        Promise_.Set(TError("Error traversing chunk tree") << error);
    }
}

////////////////////////////////////////////////////////////////////////////////

TChunkIdsAttributeVisitor::TChunkIdsAttributeVisitor(
    NCellMaster::TBootstrap* bootstrap,
    TChunkList* chunkList)
    : TChunkVisitorBase(bootstrap, chunkList)
    , Writer_(&Stream_)
{
    Writer_.OnBeginList();
}

bool TChunkIdsAttributeVisitor::OnChunk(
    TChunk* chunk,
    std::optional<i64> /*rowIndex*/,
    std::optional<int> /*tabletIndex*/,
    const TLegacyReadLimit& /*startLimit*/,
    const TLegacyReadLimit& /*endLimit*/,
    TTransactionId /*timestampTransactionId*/)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Writer_.OnListItem();
    Writer_.OnStringScalar(ToString(chunk->GetId()));

    return true;
}

bool TChunkIdsAttributeVisitor::OnChunkView(TChunkView* /*chunkView*/)
{
    return false;
}

bool TChunkIdsAttributeVisitor::OnDynamicStore(
    TDynamicStore* /*dynamicStore*/,
    std::optional<int> /*tabletIndex*/,
    const NChunkClient::TLegacyReadLimit& /*startLimit*/,
    const NChunkClient::TLegacyReadLimit& /*endLimit*/)
{
    return true;
}

void TChunkIdsAttributeVisitor::OnSuccess()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Writer_.OnEndList();
    Writer_.Flush();
    Promise_.Set(TYsonString(Stream_.Str()));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
