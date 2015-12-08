#pragma once

#include "public.h"
#include "session_detail.h"

#include <yt/server/cell_node/public.h>

#include <yt/server/hydra/public.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

class TJournalSession
    : public TSessionBase
{
public:
    TJournalSession(
        TDataNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap,
        const TChunkId& chunkId,
        const TSessionOptions& options,
        TStoreLocationPtr location,
        TLease lease);

    virtual NChunkClient::NProto::TChunkInfo GetChunkInfo() const override;

private:
    TJournalChunkPtr Chunk_;
    TFuture<void> LastAppendResult_ = VoidFuture;


    virtual TFuture<void> DoStart() override;

    virtual TFuture<void> DoPutBlocks(
        int startBlockIndex,
        const std::vector<TSharedRef>& blocks,
        bool enableCaching) override;

    virtual TFuture<void> DoSendBlocks(
        int startBlockIndex,
        int blockCount,
        const NNodeTrackerClient::TNodeDescriptor& target) override;

    virtual TFuture<void> DoFlushBlocks(int blockIndex) override;

    virtual void DoCancel() override;

    virtual TFuture<IChunkPtr> DoFinish(
        const NChunkClient::NProto::TChunkMeta* chunkMeta,
        const TNullable<int>& blockCount) override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

