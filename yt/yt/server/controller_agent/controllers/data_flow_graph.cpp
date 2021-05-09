#include "data_flow_graph.h"

#include <yt/yt/server/controller_agent/virtual.h>

#include <yt/yt/server/lib/chunk_pools/chunk_pool.h>
#include <yt/yt/server/lib/chunk_pools/input_chunk_mapping.h>

#include <yt/yt/server/lib/controller_agent/serialize.h>

#include <yt/yt/client/chunk_client/data_statistics.h>
#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/ytlib/chunk_client/input_chunk.h>

#include <yt/yt/ytlib/table_client/config.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/virtual.h>

#include <util/generic/cast.h>

namespace NYT::NControllerAgent::NControllers {

using namespace NYTree;
using namespace NChunkClient::NProto;
using namespace NChunkClient;
using namespace NTableClient;
using namespace NYson;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

using TVertexDescriptor = TString;

TString TDataFlowGraph::SourceDescriptor("source");
TString TDataFlowGraph::SinkDescriptor("sink");

DECLARE_REFCOUNTED_CLASS(TVertex)
DECLARE_REFCOUNTED_CLASS(TEdge)
DECLARE_REFCOUNTED_CLASS(TLivePreview)

////////////////////////////////////////////////////////////////////////////////

void TStreamDescriptor::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, ChunkMapping);
    Persist(context, DestinationPool);
    Persist(context, RequiresRecoveryInfo);
    Persist(context, TableWriterOptions);
    Persist(context, TableUploadOptions);
    Persist(context, TableWriterConfig);
    Persist(context, Timestamp);
    Persist(context, CellTags);
    Persist(context, ImmediatelyUnstageChunkLists);
    Persist(context, IsOutputTableDynamic);
    Persist(context, IsFinalOutput);
    Persist(context, LivePreviewIndex);
    Persist(context, TargetDescriptor);
    Persist(context, PartitionTag);
    Persist<TVectorSerializer<TNonNullableIntrusivePtrSerializer<>>>(context, StreamSchemas);
}

TStreamDescriptor& TStreamDescriptor::operator =(const TStreamDescriptor& other)
{
    DestinationPool = other.DestinationPool;
    ChunkMapping = other.ChunkMapping;
    RequiresRecoveryInfo = other.RequiresRecoveryInfo;
    TableWriterOptions = CloneYsonSerializable(other.TableWriterOptions);
    TableUploadOptions = other.TableUploadOptions;
    TableWriterConfig = other.TableWriterConfig;
    Timestamp = other.Timestamp;
    CellTags = other.CellTags;
    ImmediatelyUnstageChunkLists = other.ImmediatelyUnstageChunkLists;
    IsFinalOutput = other.IsFinalOutput;
    IsOutputTableDynamic = other.IsOutputTableDynamic;
    LivePreviewIndex = other.LivePreviewIndex;
    TargetDescriptor = other.TargetDescriptor;
    StreamSchemas = other.StreamSchemas;

    return *this;
}

////////////////////////////////////////////////////////////////////////////////

class TLivePreview
    : public TRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(NYTree::IYPathServicePtr, Service);
    DEFINE_BYREF_RW_PROPERTY(THashSet<NChunkClient::TInputChunkPtr>, Chunks);

public:
    TLivePreview() = default;

    explicit TLivePreview(TNodeDirectoryPtr nodeDirectory)
        : NodeDirectory_(std::move(nodeDirectory))
    {
        Initialize();
    }

    void Persist(const TPersistenceContext& context)
    {
        using NYT::Persist;

        Persist<TSetSerializer<TDefaultSerializer, TUnsortedTag>>(context, Chunks_);
        Persist(context, NodeDirectory_);

        if (context.IsLoad()) {
            Initialize();
        }
    }

private:
    TNodeDirectoryPtr NodeDirectory_;

    void Initialize()
    {
        Service_ = New<TVirtualStaticTable>(Chunks_, NodeDirectory_);
    }
};

DEFINE_REFCOUNTED_TYPE(TLivePreview)

////////////////////////////////////////////////////////////////////////////////

class TEdge
    : public TRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(IYPathServicePtr, Service);
    DEFINE_BYREF_RW_PROPERTY(TVertexDescriptor, SourceName);
    DEFINE_BYREF_RW_PROPERTY(TVertexDescriptor, TargetName);
    DEFINE_BYREF_RW_PROPERTY(TDataStatistics, JobDataStatistics);
    DEFINE_BYREF_RW_PROPERTY(TDataStatistics, TeleportDataStatistics);

public:
    //! For persistence only.
    TEdge() = default;

    TEdge(TVertexDescriptor sourceName, TVertexDescriptor targetName)
        : SourceName_(std::move(sourceName))
        , TargetName_(std::move(targetName))
    {
        Initialize();
    }

    void Persist(const TPersistenceContext& context)
    {
        using NYT::Persist;

        Persist(context, SourceName_);
        Persist(context, TargetName_);
        Persist(context, JobDataStatistics_);
        Persist(context, TeleportDataStatistics_);

        if (context.IsLoad()) {
            Initialize();
        }
    }

    void BuildDirectionYson(TFluentMap fluent)
    {
        auto getVertexName = [] (const TVertexDescriptor& descriptor) -> TString {
            if (descriptor == TDataFlowGraph::SourceDescriptor) {
                return "input";
            } else if (descriptor == TDataFlowGraph::SinkDescriptor) {
                return "output";
            } else {
                return descriptor;
            }
        };

        fluent
            .Item("source_name").Value(getVertexName(SourceName_))
            .Item("target_name").Value(getVertexName(TargetName_))
            .Item("job_data_statistics").Value(JobDataStatistics_)
            .Item("teleport_data_statistics").Value(TeleportDataStatistics_);
    }

private:
    void Initialize()
    {
        auto service = New<TCompositeMapService>()
            // COMPAT(gritukan): Drop it in favour of job_data_statistics.
            ->AddChild("statistics", IYPathService::FromProducer(BIND([weakThis = MakeWeak(this)] (IYsonConsumer* consumer) {
                if (auto this_ = weakThis.Lock()) {
                    BuildYsonFluently(consumer)
                        .Value(this_->JobDataStatistics_ + this_->TeleportDataStatistics_);
                }
            })))
            ->AddChild("job_data_statistics", IYPathService::FromProducer(BIND([weakThis = MakeWeak(this)] (IYsonConsumer* consumer) {
                if (auto this_ = weakThis.Lock()) {
                    BuildYsonFluently(consumer)
                        .Value(this_->JobDataStatistics_);
                }
            })))
            ->AddChild("teleport_data_statistics", IYPathService::FromProducer(BIND([weakThis = MakeWeak(this)] (IYsonConsumer* consumer) {
                if (auto this_ = weakThis.Lock()) {
                    BuildYsonFluently(consumer)
                        .Value(this_->TeleportDataStatistics_);
                }
            })));

        service->SetOpaque(false);
        Service_ = std::move(service);
    }
};

DEFINE_REFCOUNTED_TYPE(TEdge)

////////////////////////////////////////////////////////////////////////////////

class TVertex
    : public TRefCounted
{
public:
    DEFINE_BYREF_RW_PROPERTY(TVertexDescriptor, VertexDescriptor);
    DEFINE_BYVAL_RO_PROPERTY(NYTree::IYPathServicePtr, Service);
    DEFINE_BYREF_RW_PROPERTY(TProgressCounterPtr, JobCounter, New<TProgressCounter>());
    DEFINE_BYVAL_RW_PROPERTY(EJobType, JobType);

    using TLivePreviewList = std::vector<TLivePreviewPtr>;
    DEFINE_BYVAL_RO_PROPERTY(std::shared_ptr<TLivePreviewList>, LivePreviews, std::make_shared<TLivePreviewList>());

    using TEdgeMap = THashMap<TVertexDescriptor, TEdgePtr>;
    DEFINE_BYREF_RO_PROPERTY(std::shared_ptr<TEdgeMap>, Edges, std::make_shared<TEdgeMap>());

public:
    //! For persistence only.
    TVertex() = default;

    TVertex(
        TVertexDescriptor vertexDescriptor,
        TNodeDirectoryPtr nodeDirectory)
        : VertexDescriptor_(std::move(vertexDescriptor))
        , NodeDirectory_(std::move(nodeDirectory))
    {
        Initialize();
    }

    const TEdgePtr& GetOrRegisterEdge(const TVertexDescriptor& to)
    {
        auto it = Edges_->find(to);
        if (it == Edges_->end()) {
            auto& edge = (*Edges_)[to];
            edge = New<TEdge>(VertexDescriptor_, to);
            return edge;
        } else {
            return it->second;
        }
    }

    void Persist(const TPersistenceContext& context)
    {
        using NYT::Persist;

        Persist(context, VertexDescriptor_);
        Persist(context, JobCounter_);
        Persist(context, JobType_);
        Persist(context, *LivePreviews_);
        Persist(context, *Edges_);
        Persist(context, NodeDirectory_);

        if (context.IsLoad()) {
            Initialize();
        }
    }

    void RegisterLivePreviewChunk(int index, TInputChunkPtr chunk)
    {
        if (index >= std::ssize(*LivePreviews_)) {
            LivePreviews_->resize(index + 1);
        }
        if (!(*LivePreviews_)[index]) {
            (*LivePreviews_)[index] = New<TLivePreview>(NodeDirectory_);
        }

        YT_VERIFY((*LivePreviews_)[index]->Chunks().insert(std::move(chunk)).second);
    }

    void UnregisterLivePreviewChunk(int index, TInputChunkPtr chunk)
    {
        YT_VERIFY(0 <= index && index < std::ssize(*LivePreviews_));
        YT_VERIFY((*LivePreviews_)[index]);

        YT_VERIFY((*LivePreviews_)[index]->Chunks().erase(std::move(chunk)));
    }

private:
    TNodeDirectoryPtr NodeDirectory_;

    void Initialize()
    {
        using TEdgeMapService = NYTree::TCollectionBoundMapService<TEdgeMap>;
        auto edgeMapService = New<TEdgeMapService>(std::weak_ptr<TEdgeMap>(Edges_));
        edgeMapService->SetOpaque(false);

        using TLivePreviewListService = NYTree::TCollectionBoundListService<TLivePreviewList>;
        auto livePreviewService = New<TLivePreviewListService>(std::weak_ptr<TLivePreviewList>(LivePreviews_));

        auto service = New<TCompositeMapService>();
        service->AddChild("edges", edgeMapService);
        service->AddChild("live_previews", livePreviewService);
        service->SetOpaque(false);

        Service_ = std::move(service);
    }
};

DEFINE_REFCOUNTED_TYPE(TVertex)

////////////////////////////////////////////////////////////////////////////////

class TDataFlowGraph::TImpl
    : public TRefCounted
{
public:
    TImpl() = default;

    TImpl(TNodeDirectoryPtr nodeDirectory)
        : NodeDirectory_(std::move(nodeDirectory))
    {
        Initialize();
    }

    IYPathServicePtr GetService() const
    {
        return Service_;
    }

    const std::vector<TVertexDescriptor>& GetTopologicalOrdering() const
    {
        return TopologicalOrdering_.GetOrdering();
    }

    const TProgressCounterPtr& GetTotalJobCounter() const
    {
        return TotalJobCounter_;
    }

    void Persist(const TPersistenceContext& context)
    {
        using NYT::Persist;

        Persist(context, TotalJobCounter_);
        Persist(context, *Vertices_);
        Persist(context, TopologicalOrdering_);
        Persist(context, NodeDirectory_);

        if (context.IsLoad()) {
            Initialize();
        }
    }

    void RegisterEdge(
        const TDataFlowGraph::TVertexDescriptor& from,
        const TDataFlowGraph::TVertexDescriptor& to)
    {
        TopologicalOrdering_.AddEdge(from, to);
        GetOrRegisterEdge(from, to);
    }

    void UpdateEdgeJobDataStatistics(
        const TDataFlowGraph::TVertexDescriptor& from,
        const TDataFlowGraph::TVertexDescriptor& to,
        const TDataStatistics& jobDataStatistics)
    {
        TopologicalOrdering_.AddEdge(from, to);
        const auto& edge = GetOrRegisterEdge(from, to);
        edge->JobDataStatistics() += jobDataStatistics;
    }

    void UpdateEdgeTeleportDataStatistics(
        const TDataFlowGraph::TVertexDescriptor& from,
        const TDataFlowGraph::TVertexDescriptor& to,
        const TDataStatistics& teleportDataStatistics)
    {
        TopologicalOrdering_.AddEdge(from, to);
        const auto& edge = GetOrRegisterEdge(from, to);
        edge->TeleportDataStatistics() += teleportDataStatistics;
    }

    void RegisterCounter(
        const TVertexDescriptor& descriptor,
        const TProgressCounterPtr& counter,
        EJobType jobType)
    {
        const auto& vertex = GetOrRegisterVertex(descriptor);
        vertex->SetJobType(jobType);
        counter->AddParent(vertex->JobCounter());
    }

    void RegisterLivePreviewChunk(const TVertexDescriptor& descriptor, int index, TInputChunkPtr chunk)
    {
        const auto& vertex = GetOrRegisterVertex(descriptor);
        vertex->RegisterLivePreviewChunk(index, std::move(chunk));
    }

    void UnregisterLivePreviewChunk(const TVertexDescriptor& descriptor, int index, TInputChunkPtr chunk)
    {
        const auto& vertex = GetOrRegisterVertex(descriptor);
        vertex->UnregisterLivePreviewChunk(index, std::move(chunk));
    }

    void BuildDataFlowYson(TFluentList fluent) const
    {
        std::vector<TEdgePtr> edges;
        for (const auto& [sourceDescriptor, vertex] : *Vertices_) {
            for (const auto& [targetDescriptor, edge] : *vertex->Edges()) {
                edges.push_back(edge);
            }
        }

        fluent
            .DoFor(edges, [&] (TFluentList fluent, const TEdgePtr& edge) {
                fluent.Item()
                    .BeginMap()
                        .Do(BIND(&TEdge::BuildDirectionYson, edge))
                    .EndMap();
            });
    }

    void BuildLegacyYson(TFluentMap fluent) const
    {
        auto topologicalOrdering = GetTopologicalOrdering();
        fluent
            .Item("vertices").BeginMap()
                .DoFor(topologicalOrdering, [&] (TFluentMap fluent, const TVertexDescriptor& vertexDescriptor) {
                    auto it = Vertices_->find(vertexDescriptor);
                    if (it != Vertices_->end()) {
                        fluent
                            .Item(vertexDescriptor).BeginMap()
                                .Item("job_counter").Value(it->second->JobCounter())
                                .Item("job_type").Value(it->second->GetJobType())
                            .EndMap();
                    }
                })
                .Item("total").BeginMap()
                    .Item("job_counter").Value(TotalJobCounter_)
                .EndMap()
            .EndMap()
            .Item("edges")
                .DoMapFor(topologicalOrdering, [&] (TFluentMap fluent, const TVertexDescriptor& from) {
                    auto it = Vertices_->find(from);
                    if (it != Vertices_->end()) {
                        fluent.Item(from)
                            .DoMapFor(*(it->second->Edges()), [&] (TFluentMap fluent, const auto& pair) {
                                auto to = pair.first;
                                const auto& edge = pair.second;
                                fluent
                                    .Item(to).BeginMap()
                                        .Item("statistics").Value(edge->JobDataStatistics() + edge->TeleportDataStatistics())
                                    .EndMap();
                            });
                    }
                })
            .Item("topological_ordering").List(topologicalOrdering);
    }

private:
    using TVertexMap = THashMap<TVertexDescriptor, TVertexPtr>;
    const std::shared_ptr<TVertexMap> Vertices_ = std::make_shared<TVertexMap>();

    TProgressCounterPtr TotalJobCounter_ = New<TProgressCounter>();

    TIncrementalTopologicalOrdering<TVertexDescriptor> TopologicalOrdering_;

    TNodeDirectoryPtr NodeDirectory_;

    NYTree::IYPathServicePtr Service_;

    void Initialize()
    {
        using TVertexMapService = TCollectionBoundMapService<TVertexMap>;

        auto vertexMapService = New<TVertexMapService>(std::weak_ptr<TVertexMap>(Vertices_));
        vertexMapService->SetOpaque(false);
        auto service = New<TCompositeMapService>()
            ->AddChild("vertices", std::move(vertexMapService))
            ->AddChild("topological_ordering", IYPathService::FromProducer(BIND([weakThis = MakeWeak(this)] (IYsonConsumer* consumer) {
                if (auto this_ = weakThis.Lock()) {
                    BuildYsonFluently(consumer)
                        .List(this_->GetTopologicalOrdering());
                }
            })));
        service->SetOpaque(false);
        Service_ = std::move(service);
    }

    const TVertexPtr& GetOrRegisterVertex(const TVertexDescriptor& descriptor)
    {
        auto it = Vertices_->find(descriptor);
        if (it == Vertices_->end()) {
            auto& vertex = (*Vertices_)[descriptor];
            vertex = New<TVertex>(descriptor, NodeDirectory_);
            vertex->JobCounter()->AddParent(TotalJobCounter_);
            return vertex;
        } else {
            return it->second;
        }
    }

    const TEdgePtr& GetOrRegisterEdge(const TVertexDescriptor& from, const TVertexDescriptor& to)
    {
        auto& vertex = GetOrRegisterVertex(from);
        return vertex->GetOrRegisterEdge(to);
    }
};

////////////////////////////////////////////////////////////////////////////////

TDataFlowGraph::TDataFlowGraph()
    : Impl_(New<TImpl>())
{ }

TDataFlowGraph::TDataFlowGraph(TNodeDirectoryPtr nodeDirectory)
    : Impl_(New<TImpl>(std::move(nodeDirectory)))
{ }

TDataFlowGraph::~TDataFlowGraph() = default;

IYPathServicePtr TDataFlowGraph::GetService() const
{
    return Impl_->GetService();
}

void TDataFlowGraph::Persist(const TPersistenceContext& context)
{
    Impl_->Persist(context);
}

void TDataFlowGraph::RegisterEdge(
    const TVertexDescriptor& from,
    const TVertexDescriptor& to)
{
    Impl_->RegisterEdge(from, to);
}

void TDataFlowGraph::UpdateEdgeJobDataStatistics(
    const TVertexDescriptor& from,
    const TVertexDescriptor& to,
    const NChunkClient::NProto::TDataStatistics& jobDataStatistics)
{
    Impl_->UpdateEdgeJobDataStatistics(from, to, jobDataStatistics);
}

void TDataFlowGraph::UpdateEdgeTeleportDataStatistics(
    const TVertexDescriptor& from,
    const TVertexDescriptor& to,
    const NChunkClient::NProto::TDataStatistics& teleportDataStatistics)
{
    Impl_->UpdateEdgeTeleportDataStatistics(from, to, teleportDataStatistics);
}

void TDataFlowGraph::RegisterCounter(
    const TVertexDescriptor& vertex,
    const TProgressCounterPtr& counter,
    EJobType jobType)
{
    Impl_->RegisterCounter(vertex, counter, jobType);
}

void TDataFlowGraph::RegisterLivePreviewChunk(const TVertexDescriptor& descriptor, int index, TInputChunkPtr chunk)
{
    Impl_->RegisterLivePreviewChunk(descriptor, index, std::move(chunk));
}

void TDataFlowGraph::UnregisterLivePreviewChunk(const TVertexDescriptor& descriptor, int index, TInputChunkPtr chunk)
{
    Impl_->UnregisterLivePreviewChunk(descriptor, index, std::move(chunk));
}

void TDataFlowGraph::BuildDataFlowYson(TFluentList fluent) const
{
    Impl_->BuildDataFlowYson(fluent);
}

void TDataFlowGraph::BuildLegacyYson(TFluentMap fluent) const
{
    Impl_->BuildLegacyYson(fluent);
}

const TProgressCounterPtr& TDataFlowGraph::GetTotalJobCounter() const
{
    return Impl_->GetTotalJobCounter();
}

const std::vector<TVertexDescriptor>& TDataFlowGraph::GetTopologicalOrdering() const
{
    return Impl_->GetTopologicalOrdering();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent::NControllers
