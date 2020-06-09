#pragma once

#include "private.h"

#include <yt/server/lib/chunk_pools/public.h>

#include <yt/ytlib/table_client/public.h>
#include <yt/ytlib/table_client/table_upload_options.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/misc/topological_ordering.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/virtual.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

class TDataFlowGraph
    : public TRefCounted
{
public:
    using TVertexDescriptor = TString;

    static TVertexDescriptor SourceDescriptor;
    static TVertexDescriptor SinkDescriptor;

    TDataFlowGraph();
    TDataFlowGraph(NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory);
    ~TDataFlowGraph();

    NYTree::IYPathServicePtr GetService() const;

    void Persist(const TPersistenceContext& context);

    void UpdateEdgeJobDataStatistics(
        const TVertexDescriptor& from,
        const TVertexDescriptor& to,
        const NChunkClient::NProto::TDataStatistics& jobDataStatistics);

    void UpdateEdgeTeleportDataStatistics(
        const TVertexDescriptor& from,
        const TVertexDescriptor& to,
        const NChunkClient::NProto::TDataStatistics& teleportDataStatistics);

    void RegisterCounter(
        const TVertexDescriptor& vertex,
        const TProgressCounterPtr& counter,
        EJobType jobType);

    void RegisterLivePreviewChunk(const TVertexDescriptor& descriptor, int index, NChunkClient::TInputChunkPtr chunk);
    void UnregisterLivePreviewChunk(const TVertexDescriptor& descriptor, int index, NChunkClient::TInputChunkPtr chunk);

    void BuildDataFlowYson(NYTree::TFluentList fluent) const;

    void BuildLegacyYson(NYTree::TFluentMap fluent) const;

    const TProgressCounterPtr& GetTotalJobCounter() const;

    const std::vector<TVertexDescriptor>& GetTopologicalOrdering() const;

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TDataFlowGraph);

////////////////////////////////////////////////////////////////////////////////

struct TEdgeDescriptor
{
    TEdgeDescriptor() = default;

    // Keep fields below in sync with operator =.
    NChunkPools::IChunkPoolInput* DestinationPool = nullptr;
    // May be null if recovery info is not required.
    NChunkPools::TInputChunkMappingPtr ChunkMapping;
    bool RequiresRecoveryInfo = false;
    NTableClient::TTableWriterOptionsPtr TableWriterOptions;
    NTableClient::TTableUploadOptions TableUploadOptions;
    NYson::TYsonString TableWriterConfig;
    std::optional<NTransactionClient::TTimestamp> Timestamp;
    // Cell tag to allocate chunk lists.
    NObjectClient::TCellTag CellTag;
    bool ImmediatelyUnstageChunkLists = false;
    bool IsFinalOutput = false;
    bool IsOutputTableDynamic = false;
    // In most situations coincides with the index of an edge descriptor,
    // but in some situations may differ. For example, an auto merge task
    // may have the only output descriptor, but we would like to attach
    // its output chunks to the live preview with an index corresponding to the
    // output table index.
    int LivePreviewIndex = 0;
    TDataFlowGraph::TVertexDescriptor TargetDescriptor;

    void Persist(const TPersistenceContext& context);

    TEdgeDescriptor& operator =(const TEdgeDescriptor& other);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
