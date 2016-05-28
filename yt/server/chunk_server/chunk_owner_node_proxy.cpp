#include "chunk_owner_node_proxy.h"
#include "private.h"
#include "chunk.h"
#include "chunk_list.h"
#include "chunk_manager.h"
#include "chunk_tree_traversing.h"
#include "config.h"
#include "helpers.h"

#include <yt/server/cell_master/config.h>
#include <yt/server/cell_master/multicell_manager.h>

#include <yt/server/node_tracker_server/node_directory_builder.h>

#include <yt/server/object_server/object.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/transaction_client/transaction_ypath.pb.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/ytree/helpers.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/node.h>
#include <yt/core/ytree/system_attribute_provider.h>

#include <type_traits>

namespace NYT {
namespace NChunkServer {

using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NChunkClient;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NCypressServer;
using namespace NNodeTrackerServer;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NYson;
using namespace NYTree;
using namespace NTableClient;

using NChunkClient::NProto::TReqFetch;
using NChunkClient::NProto::TRspFetch;
using NChunkClient::NProto::TMiscExt;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

//! Adds #cellTag into #cellTags if the former is not a sentinel.
void InsertCellTag(TCellTagList* cellTags, TCellTag cellTag)
{
    if (cellTag >= MinValidCellTag && cellTag <= MaxValidCellTag) {
        cellTags->push_back(cellTag);
    }
}

//! Removes #cellTag from #cellTags if the former is present there.
void RemoveCellTag(TCellTagList* cellTags, TCellTag cellTag)
{
    cellTags->erase(
        std::remove(cellTags->begin(), cellTags->end(), cellTag),
        cellTags->end());
}

//! Sorts and removes duplicates from #cellTags.
void CanonizeCellTags(TCellTagList* cellTags)
{
    std::sort(cellTags->begin(), cellTags->end());
    cellTags->erase(
        std::unique(cellTags->begin(), cellTags->end()),
        cellTags->end());
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TFetchChunkVisitor
    : public IChunkVisitor
{
public:
    typedef NRpc::TTypedServiceContext<TReqFetch, TRspFetch> TCtxFetch;
    typedef TIntrusivePtr<TCtxFetch> TCtxFetchPtr;

    TFetchChunkVisitor(
        NCellMaster::TBootstrap* bootstrap,
        TChunkManagerConfigPtr config,
        TChunkList* chunkList,
        TCtxFetchPtr context,
        const TChannel& channel,
        bool fetchParityReplicas,
        const std::vector<TReadRange>& ranges)
        : Bootstrap_(bootstrap)
        , Config_(config)
        , ChunkList_(chunkList)
        , Context_(context)
        , Channel_(channel)
        , FetchParityReplicas_(fetchParityReplicas)
        , Ranges_(ranges)
        , NodeDirectoryBuilder_(context->Response().mutable_node_directory())
    {
        if (!Context_->Request().fetch_all_meta_extensions()) {
            for (int tag : Context_->Request().extension_tags()) {
                ExtensionTags_.insert(tag);
            }
        }
    }

    void Run()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (Ranges_.empty()) {
            ReplySuccess();
            return;
        }

        TraverseCurrentRange();
    }

private:
    NCellMaster::TBootstrap* const Bootstrap_;
    const TChunkManagerConfigPtr Config_;
    TChunkList* const ChunkList_;
    const TCtxFetchPtr Context_;
    const TChannel Channel_;
    const bool FetchParityReplicas_;

    std::vector<TReadRange> Ranges_;
    int CurrentRangeIndex_ = 0;

    yhash_set<int> ExtensionTags_;
    TNodeDirectoryBuilder NodeDirectoryBuilder_;
    bool Finished_ = false;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    void TraverseCurrentRange()
    {
        TraverseChunkTree(
            CreatePreemptableChunkTraverserCallbacks(Bootstrap_),
            this,
            ChunkList_,
            Ranges_[CurrentRangeIndex_].LowerLimit(),
            Ranges_[CurrentRangeIndex_].UpperLimit());
    }

    void ReplySuccess()
    {
        YCHECK(!Finished_);
        Finished_ = true;

        try {
            // Update upper limits for all returned journal chunks.
            auto* chunkSpecs = Context_->Response().mutable_chunks();
            auto chunkManager = Bootstrap_->GetChunkManager();
            for (auto& chunkSpec : *chunkSpecs) {
                auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
                if (TypeFromId(chunkId) == EObjectType::JournalChunk) {
                    auto* chunk = chunkManager->FindChunk(chunkId);
                    if (!IsObjectAlive(chunk)) {
                        THROW_ERROR_EXCEPTION(
                            NRpc::EErrorCode::Unavailable,
                            "Optimistic locking failed for chunk %v",
                            chunkId);
                    }

                    auto result = WaitFor(chunkManager->GetChunkQuorumInfo(chunk))
                        .ValueOrThrow();
                    i64 quorumRowCount = result.row_count();

                    auto lowerLimit = FromProto<TReadLimit>(chunkSpec.lower_limit());
                    if (!lowerLimit.HasRowIndex()) {
                        lowerLimit.SetRowIndex(0);
                    }
                    ToProto(chunkSpec.mutable_lower_limit(), lowerLimit);

                    auto upperLimit = FromProto<TReadLimit>(chunkSpec.upper_limit());
                    i64 upperLimitRowIndex = upperLimit.HasRowIndex() ? upperLimit.GetRowIndex() : std::numeric_limits<i64>::max();
                    upperLimit.SetRowIndex(std::min(upperLimitRowIndex, quorumRowCount));
                    ToProto(chunkSpec.mutable_upper_limit(), upperLimit);
                }
            }

            Context_->SetResponseInfo("ChunkCount: %v", chunkSpecs->size());
            Context_->Reply();
        } catch (const std::exception& ex) {
            Context_->Reply(ex);
        }
    }

    void ReplyError(const TError& error)
    {
        if (Finished_)
            return;

        Finished_ = true;

        Context_->Reply(error);
    }

    virtual bool OnChunk(
        TChunk* chunk,
        i64 rowIndex,
        const TReadLimit& lowerLimit,
        const TReadLimit& upperLimit) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (Context_->Response().chunks_size() >= Config_->MaxChunksPerFetch) {
            ReplyError(TError("Attempt to fetch too many chunks in a single request")
                << TErrorAttribute("limit", Config_->MaxChunksPerFetch));
            return false;
        }

        auto chunkManager = Bootstrap_->GetChunkManager();
        const auto& config = Bootstrap_->GetConfig()->ChunkManager;

        if (!chunk->IsConfirmed()) {
            ReplyError(TError("Cannot fetch an object containing an unconfirmed chunk %v",
                chunk->GetId()));
            return false;
        }

        auto* chunkSpec = Context_->Response().add_chunks();

        chunkSpec->set_table_row_index(rowIndex);

        if (!Channel_.IsUniversal()) {
            ToProto(chunkSpec->mutable_channel(), Channel_);
        }

        auto erasureCodecId = chunk->GetErasureCodec();
        int firstInfeasibleReplicaIndex =
            erasureCodecId == NErasure::ECodec::None || FetchParityReplicas_
                ? std::numeric_limits<int>::max() // all replicas are feasible
                : NErasure::GetCodec(erasureCodecId)->GetDataPartCount();

        SmallVector<TNodePtrWithIndex, TypicalReplicaCount> replicas;
        auto addReplica = [&] (TNodePtrWithIndex replica) -> bool {
            if (replica.GetIndex() < firstInfeasibleReplicaIndex) {
                replicas.push_back(replica);
                return true;
            } else {
                return false;
            }
        };

        for (auto replica : chunk->StoredReplicas()) {
            addReplica(replica);
        }

        int cachedReplicaCount = 0;
        for (auto replica : chunk->CachedReplicas()) {
            if (cachedReplicaCount >= config->MaxCachedReplicasPerFetch) {
                break;
            }
            if (addReplica(replica)) {
                ++cachedReplicaCount;
            }
        }

        for (auto replica : replicas) {
            NodeDirectoryBuilder_.Add(replica);
            chunkSpec->add_replicas(NYT::ToProto<ui32>(replica));
        }

        ToProto(chunkSpec->mutable_chunk_id(), chunk->GetId());
        chunkSpec->set_erasure_codec(static_cast<int>(erasureCodecId));

        chunkSpec->mutable_chunk_meta()->set_type(chunk->ChunkMeta().type());
        chunkSpec->mutable_chunk_meta()->set_version(chunk->ChunkMeta().version());

        if (Context_->Request().fetch_all_meta_extensions()) {
            *chunkSpec->mutable_chunk_meta()->mutable_extensions() = chunk->ChunkMeta().extensions();
        } else {
            FilterProtoExtensions(
                chunkSpec->mutable_chunk_meta()->mutable_extensions(),
                chunk->ChunkMeta().extensions(),
                ExtensionTags_);
        }

        // Try to keep responses small -- avoid producing redundant limits.
        if (!IsTrivial(lowerLimit)) {
            ToProto(chunkSpec->mutable_lower_limit(), lowerLimit);
        }
        if (!IsTrivial(upperLimit)) {
            ToProto(chunkSpec->mutable_upper_limit(), upperLimit);
        }

        chunkSpec->set_range_index(CurrentRangeIndex_);

        return true;
    }

    virtual void OnError(const TError& error) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ReplyError(error);
    }

    virtual void OnFinish() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (Finished_) {
            return;
        }

        ++CurrentRangeIndex_;
        if (CurrentRangeIndex_ == Ranges_.size()) {
            ReplySuccess();
        } else {
            TraverseCurrentRange();
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkVisitorBase
    : public IChunkVisitor
{
public:
    TFuture<TYsonString> Run()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TraverseChunkTree(
            CreatePreemptableChunkTraverserCallbacks(Bootstrap_),
            this,
            ChunkList_);

        return Promise_;
    }

protected:
    NCellMaster::TBootstrap* const Bootstrap_;
    TChunkList* const ChunkList_;

    TPromise<TYsonString> Promise_ = NewPromise<TYsonString>();

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    TChunkVisitorBase(
        NCellMaster::TBootstrap* bootstrap,
        TChunkList* chunkList)
        : Bootstrap_(bootstrap)
        , ChunkList_(chunkList)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
    }

    virtual void OnError(const TError& error) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        Promise_.Set(TError("Error traversing chunk tree") << error);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkIdsAttributeVisitor
    : public TChunkVisitorBase
{
public:
    TChunkIdsAttributeVisitor(
        NCellMaster::TBootstrap* bootstrap,
        TChunkList* chunkList)
        : TChunkVisitorBase(bootstrap, chunkList)
        , Writer_(&Stream_)
    {
        Writer_.OnBeginList();
    }

private:
    TStringStream Stream_;
    TBufferedBinaryYsonWriter Writer_;

    virtual bool OnChunk(
        TChunk* chunk,
        i64 /*rowIndex*/,
        const TReadLimit& /*startLimit*/,
        const TReadLimit& /*endLimit*/) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        Writer_.OnListItem();
        Writer_.OnStringScalar(ToString(chunk->GetId()));

        return true;
    }

    virtual void OnFinish() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        Writer_.OnEndList();
        Writer_.Flush();
        Promise_.Set(TYsonString(Stream_.Str()));
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TKeyExtractor>
class TChunkStatisticsVisitor
    : public TChunkVisitorBase
{
public:
    TChunkStatisticsVisitor(
        NCellMaster::TBootstrap* bootstrap,
        TChunkList* chunkList,
        TKeyExtractor keyExtractor)
        : TChunkVisitorBase(bootstrap, chunkList)
        , KeyExtractor_(keyExtractor)
    { }

private:
    const TKeyExtractor KeyExtractor_;

    using TKey = typename std::result_of<TKeyExtractor(const TChunk*)>::type;
    using TStatiticsMap = yhash_map<TKey, TChunkTreeStatistics>;
    TStatiticsMap StatisticsMap_;

    virtual bool OnChunk(
        TChunk* chunk,
        i64 /*rowIndex*/,
        const TReadLimit& /*startLimit*/,
        const TReadLimit& /*endLimit*/) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        StatisticsMap_[KeyExtractor_(chunk)].Accumulate(chunk->GetStatistics());
        return true;
    }

    virtual void OnFinish() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto result = BuildYsonStringFluently()
            .DoMapFor(StatisticsMap_, [=] (TFluentMap fluent, const typename TStatiticsMap::value_type& pair) {
                const auto& statistics = pair.second;
                // TODO(panin): maybe use here the same method as in attributes
                fluent
                    .Item(FormatKey(pair.first)).BeginMap()
                        .Item("chunk_count").Value(statistics.ChunkCount)
                        .Item("uncompressed_data_size").Value(statistics.UncompressedDataSize)
                        .Item("compressed_data_size").Value(statistics.CompressedDataSize)
                    .EndMap();
            });
        Promise_.Set(result);
    }


    template <class T>
    static Stroka FormatKey(T value, typename TEnumTraits<T>::TType* = 0)
    {
        return FormatEnum(value);
    }

    static Stroka FormatKey(TCellTag value)
    {
        return ToString(value);
    }
};

namespace {

template <class TKeyExtractor>
TFuture<TYsonString> ComputeChunkStatistics(
    NCellMaster::TBootstrap* bootstrap,
    TChunkList* chunkList,
    TKeyExtractor keyExtractor)
{
    auto visitor = New<TChunkStatisticsVisitor<TKeyExtractor>>(
        bootstrap,
        chunkList,
        keyExtractor);
    return visitor->Run();
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TChunkOwnerNodeProxy::TChunkOwnerNodeProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TChunkOwnerBase* trunkNode)
    : TNontemplateCypressNodeProxyBase(
        bootstrap,
        metadata,
        transaction,
        trunkNode)
{ }

ENodeType TChunkOwnerNodeProxy::GetType() const
{
    return ENodeType::Entity;
}

bool TChunkOwnerNodeProxy::DoInvoke(NRpc::IServiceContextPtr context)
{
    DISPATCH_YPATH_HEAVY_SERVICE_METHOD(Fetch);
    DISPATCH_YPATH_SERVICE_METHOD(BeginUpload);
    DISPATCH_YPATH_SERVICE_METHOD(GetUploadParams);
    DISPATCH_YPATH_SERVICE_METHOD(EndUpload);
    return TNontemplateCypressNodeProxyBase::DoInvoke(context);
}

void TChunkOwnerNodeProxy::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    TNontemplateCypressNodeProxyBase::ListSystemAttributes(descriptors);

    const auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    auto isExternal = node->IsExternal();

    descriptors->push_back(TAttributeDescriptor("chunk_list_id")
        .SetExternal(isExternal)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("chunk_ids")
        .SetExternal(isExternal)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("compression_statistics")
        .SetExternal(isExternal)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("erasure_statistics")
        .SetExternal(isExternal)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("multicell_statistics")
        .SetExternal(isExternal)
        .SetOpaque(true));
    descriptors->push_back("chunk_count");
    descriptors->push_back("uncompressed_data_size");
    descriptors->push_back("compressed_data_size");
    descriptors->push_back("compression_ratio");
    descriptors->push_back(TAttributeDescriptor("compression_codec")
        .SetCustom(true));
    descriptors->push_back(TAttributeDescriptor("erasure_codec")
        .SetCustom(true));
    descriptors->push_back("update_mode");
    descriptors->push_back(TAttributeDescriptor("replication_factor")
        .SetReplicated(true));
    descriptors->push_back(TAttributeDescriptor("vital")
        .SetReplicated(true));
}

bool TChunkOwnerNodeProxy::GetBuiltinAttribute(
    const Stroka& key,
    IYsonConsumer* consumer)
{
    auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    const auto* chunkList = node->GetChunkList();
    auto statistics = node->ComputeTotalStatistics();
    auto isExternal = node->IsExternal();

    if (!isExternal) {
        if (key == "chunk_list_id") {
            BuildYsonFluently(consumer)
                .Value(chunkList->GetId());
            return true;
        }
    }

    if (key == "chunk_count") {
        BuildYsonFluently(consumer)
            .Value(statistics.chunk_count());
        return true;
    }

    if (key == "uncompressed_data_size") {
        BuildYsonFluently(consumer)
            .Value(statistics.uncompressed_data_size());
        return true;
    }

    if (key == "compressed_data_size") {
        BuildYsonFluently(consumer)
            .Value(statistics.compressed_data_size());
        return true;
    }

    if (key == "compression_ratio") {
        double ratio = statistics.uncompressed_data_size() > 0
            ? static_cast<double>(statistics.compressed_data_size()) / statistics.uncompressed_data_size()
            : 0;
        BuildYsonFluently(consumer)
            .Value(ratio);
        return true;
    }

    if (key == "update_mode") {
        BuildYsonFluently(consumer)
            .Value(FormatEnum(node->GetUpdateMode()));
        return true;
    }

    if (key == "replication_factor") {
        BuildYsonFluently(consumer)
            .Value(node->GetReplicationFactor());
        return true;
    }

    if (key == "vital") {
        BuildYsonFluently(consumer)
            .Value(node->GetVital());
        return true;
    }

    return TNontemplateCypressNodeProxyBase::GetBuiltinAttribute(key, consumer);
}

TFuture<TYsonString> TChunkOwnerNodeProxy::GetBuiltinAttributeAsync(const Stroka& key)
{
    auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    auto* chunkList = node->GetChunkList();
    auto isExternal = node->IsExternal();

    if (!isExternal) {
        if (key == "chunk_ids") {
            auto visitor = New<TChunkIdsAttributeVisitor>(
                Bootstrap_,
                chunkList);
            return visitor->Run();
        }

        if (key == "compression_statistics") {
            return ComputeChunkStatistics(
                Bootstrap_,
                chunkList,
                [] (const TChunk* chunk) { return NCompression::ECodec(chunk->MiscExt().compression_codec()); });
        }

        if (key == "erasure_statistics") {
            return ComputeChunkStatistics(
                Bootstrap_,
                chunkList,
                [] (const TChunk* chunk) { return chunk->GetErasureCodec(); });
        }

        if (key == "multicell_statistics") {
            return ComputeChunkStatistics(
                Bootstrap_,
                chunkList,
                [] (const TChunk* chunk) { return CellTagFromId(chunk->GetId()); });
        }
    }

    return TNontemplateCypressNodeProxyBase::GetBuiltinAttributeAsync(key);
}

void TChunkOwnerNodeProxy::ValidateCustomAttributeUpdate(
    const Stroka& key,
    const TNullable<TYsonString>& /*oldValue*/,
    const TNullable<TYsonString>& newValue)
{
    if (key == "compression_codec") {
        if (!newValue) {
            ThrowCannotRemoveAttribute(key);
        }
        ConvertTo<NCompression::ECodec>(*newValue);
        return;
    }

    if (key == "erasure_codec") {
        if (!newValue) {
            ThrowCannotRemoveAttribute(key);
        }
        ConvertTo<NErasure::ECodec>(*newValue);
        return;
    }
}

bool TChunkOwnerNodeProxy::SetBuiltinAttribute(
    const Stroka& key,
    const TYsonString& value)
{
    auto chunkManager = Bootstrap_->GetChunkManager();

    auto* node = GetThisTypedImpl<TChunkOwnerBase>();

    if (key == "replication_factor") {
        ValidateNoTransaction();
        int replicationFactor = ConvertTo<int>(value);
        if (replicationFactor < MinReplicationFactor ||
            replicationFactor > MaxReplicationFactor)
        {
            THROW_ERROR_EXCEPTION("\"replication_factor\" must be in range [%v,%v]",
                MinReplicationFactor,
                MaxReplicationFactor);
        }

        YCHECK(node->IsTrunk());

        if (node->GetReplicationFactor() != replicationFactor) {
            node->SetReplicationFactor(replicationFactor);

            auto securityManager = Bootstrap_->GetSecurityManager();
            securityManager->UpdateAccountNodeUsage(node);

            if (!node->IsExternal()) {
                chunkManager->ScheduleChunkPropertiesUpdate(node->GetChunkList());
            }
        }
        return true;
    }

    if (key == "vital") {
        ValidateNoTransaction();
        bool vital = ConvertTo<bool>(value);

        YCHECK(node->IsTrunk());

        if (node->GetVital() != vital) {
            node->SetVital(vital);

            if (!node->IsExternal()) {
                chunkManager->ScheduleChunkPropertiesUpdate(node->GetChunkList());
            }
        }

        return true;
    }

    return TNontemplateCypressNodeProxyBase::SetBuiltinAttribute(key, value);
}

void TChunkOwnerNodeProxy::ValidateFetchParameters(
    const TChannel& /*channel*/,
    const std::vector<TReadRange>& /*ranges*/)
{ }

void TChunkOwnerNodeProxy::ValidateInUpdate()
{
    auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    if (node->GetUpdateMode() == EUpdateMode::None) {
        THROW_ERROR_EXCEPTION("Node is not in an update mode");
    }
}

void TChunkOwnerNodeProxy::ValidateBeginUpload()
{ }

void TChunkOwnerNodeProxy::ValidateFetch()
{ }

DEFINE_YPATH_SERVICE_METHOD(TChunkOwnerNodeProxy, Fetch)
{
    DeclareNonMutating();

    context->SetRequestInfo();

    // NB: No need for a permission check;
    // the client must have invoked GetBasicAttributes.

    ValidateNotExternal();
    ValidateFetch();

    auto channel = request->has_channel()
        ? NYT::FromProto<TChannel>(request->channel())
        : TChannel::Universal();
    bool fetchParityReplicas = request->fetch_parity_replicas();

    auto ranges = FromProto<std::vector<TReadRange>>(request->ranges());
    ValidateFetchParameters(channel, ranges);

    const auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    auto* chunkList = node->GetChunkList();

    auto visitor = New<TFetchChunkVisitor>(
        Bootstrap_,
        Bootstrap_->GetConfig()->ChunkManager,
        chunkList,
        context,
        channel,
        fetchParityReplicas,
        ranges);

    visitor->Run();
}

DEFINE_YPATH_SERVICE_METHOD(TChunkOwnerNodeProxy, BeginUpload)
{
    DeclareMutating();

    auto updateMode = EUpdateMode(request->update_mode());
    YCHECK(updateMode == EUpdateMode::Append ||
           updateMode == EUpdateMode::Overwrite);

    auto lockMode = ELockMode(request->lock_mode());
    YCHECK(lockMode == ELockMode::Shared ||
           lockMode == ELockMode::Exclusive);

    auto uploadTransactionTitle = request->has_upload_transaction_title()
        ? MakeNullable(request->upload_transaction_title())
        : Null;

    auto uploadTransactionTimeout = request->has_upload_transaction_timeout()
        ? MakeNullable(FromProto<TDuration>(request->upload_transaction_timeout()))
        : Null;

    auto uploadTransactionIdHint = request->has_upload_transaction_id()
        ? FromProto<TTransactionId>(request->upload_transaction_id())
        : NullTransactionId;

    auto uploadTransactionSecondaryCellTags =
        FromProto<TCellTagList>(request->upload_transaction_secondary_cell_tags());

    auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    auto externalCellTag = node->GetExternalCellTag();

    // Make sure |uploadTransactionSecondaryCellTags| contains the external cell tag,
    // does not contain the primary cell tag, is sorted, and contains no duplicates.
    InsertCellTag(&uploadTransactionSecondaryCellTags, externalCellTag);
    CanonizeCellTags(&uploadTransactionSecondaryCellTags);
    RemoveCellTag(&uploadTransactionSecondaryCellTags, Bootstrap_->GetPrimaryCellTag());

    // Construct |uploadTransactionReplicationCellTags| containing the tags of cells
    // the upload transaction must be replicated to. This list never contains
    // the external cell tag.
    auto uploadTransactionReplicationCellTags = uploadTransactionSecondaryCellTags;
    RemoveCellTag(&uploadTransactionReplicationCellTags, externalCellTag);

    context->SetRequestInfo(
        "UpdateMode: %v, LockMode: %v, "
        "Title: %v, Timeout: %v, SecondaryCellTags: %v",
        updateMode,
        lockMode,
        uploadTransactionTitle,
        uploadTransactionTimeout,
        uploadTransactionSecondaryCellTags);

    // NB: No need for a permission check;
    // the client must have invoked GetBasicAttributes.

    ValidateBeginUpload();

    auto chunkManager = Bootstrap_->GetChunkManager();
    auto objectManager = Bootstrap_->GetObjectManager();
    auto cypressManager = Bootstrap_->GetCypressManager();
    auto transactionManager = Bootstrap_->GetTransactionManager();

    auto* uploadTransaction = transactionManager->StartTransaction(
        Transaction,
        uploadTransactionSecondaryCellTags,
        uploadTransactionTimeout,
        uploadTransactionTitle,
        uploadTransactionIdHint);

    uploadTransaction->SetAccountingEnabled(node->GetAccountingEnabled());

    auto* lockedNode = static_cast<TChunkOwnerBase*>(cypressManager->LockNode(
        TrunkNode,
        uploadTransaction,
        lockMode));

    auto securityManager = Bootstrap_->GetSecurityManager();
    securityManager->SetNodeResourceAccounting(lockedNode, false);

    switch (updateMode) {
        case EUpdateMode::Append: {
            if (node->IsExternal() || node->GetType() == EObjectType::Journal) {
                LOG_DEBUG_UNLESS(
                    IsRecovery(),
                    "Node is switched to \"append\" mode (NodeId: %v)",
                    lockedNode->GetId());

            } else {
                auto* snapshotChunkList = lockedNode->GetChunkList();

                auto* newChunkList = chunkManager->CreateChunkList();
                newChunkList->AddOwningNode(lockedNode);

                snapshotChunkList->RemoveOwningNode(lockedNode);
                lockedNode->SetChunkList(newChunkList);
                objectManager->RefObject(newChunkList);

                chunkManager->AttachToChunkList(newChunkList, snapshotChunkList);

                auto* deltaChunkList = chunkManager->CreateChunkList();
                chunkManager->AttachToChunkList(newChunkList, deltaChunkList);

                objectManager->UnrefObject(snapshotChunkList);

                LOG_DEBUG_UNLESS(
                    IsRecovery(),
                    "Node is switched to \"append\" mode (NodeId: %v, NewChunkListId: %v, SnapshotChunkListId: %v, DeltaChunkListId: %v)",
                    node->GetId(),
                    newChunkList->GetId(),
                    snapshotChunkList->GetId(),
                    deltaChunkList->GetId());

            }
            break;
        }

        case EUpdateMode::Overwrite: {
            if (node->IsExternal() || node->GetType() == EObjectType::Journal) {
                LOG_DEBUG_UNLESS(
                    IsRecovery(),
                    "Node is switched to \"overwrite\" mode (NodeId: %v)",
                    node->GetId());
            } else {
                auto* oldChunkList = lockedNode->GetChunkList();
                oldChunkList->RemoveOwningNode(lockedNode);
                objectManager->UnrefObject(oldChunkList);

                auto* newChunkList = chunkManager->CreateChunkList();
                newChunkList->AddOwningNode(lockedNode);
                lockedNode->SetChunkList(newChunkList);
                objectManager->RefObject(newChunkList);

                LOG_DEBUG_UNLESS(
                    IsRecovery(),
                    "Node is switched to \"overwrite\" mode (NodeId: %v, NewChunkListId: %v)",
                    node->GetId(),
                    newChunkList->GetId());
            }
            break;
        }

        default:
            YUNREACHABLE();
    }

    lockedNode->BeginUpload(updateMode);

    const auto& uploadTransactionId = uploadTransaction->GetId();
    ToProto(response->mutable_upload_transaction_id(), uploadTransactionId);

    response->set_cell_tag(externalCellTag == NotReplicatedCellTag ? Bootstrap_->GetPrimaryCellTag() : externalCellTag);

    auto multicellManager = Bootstrap_->GetMulticellManager();

    if (node->IsExternal()) {
        auto replicationRequest = TChunkOwnerYPathProxy::BeginUpload(FromObjectId(GetId()));
        replicationRequest->set_update_mode(static_cast<int>(updateMode));
        replicationRequest->set_lock_mode(static_cast<int>(lockMode));
        ToProto(replicationRequest->mutable_upload_transaction_id(), uploadTransactionId);
        if (uploadTransactionTitle) {
            replicationRequest->set_upload_transaction_title(*uploadTransactionTitle);
        }
        // NB: upload_transaction_timeout must be null
        // NB: upload_transaction_secondary_cell_tags must be empty
        SetTransactionId(replicationRequest, GetObjectId(GetTransaction()));

        multicellManager->PostToMaster(replicationRequest, externalCellTag);
    }

    if (!uploadTransactionReplicationCellTags.empty()) {
        NObjectServer::NProto::TReqCreateForeignObject replicationRequest;
        ToProto(replicationRequest.mutable_object_id(), uploadTransactionId);
        replicationRequest.set_type(static_cast<int>(EObjectType::Transaction));
        auto* reqExt = replicationRequest.mutable_extensions()->MutableExtension(NTransactionClient::NProto::TTransactionCreationExt::transaction_creation_ext);
        if (Transaction) {
            ToProto(reqExt->mutable_parent_id(), Transaction->GetId());
        }

        multicellManager->PostToMasters(replicationRequest, uploadTransactionReplicationCellTags);
    }

    context->SetResponseInfo("UploadTransactionId: %v", uploadTransactionId);
    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TChunkOwnerNodeProxy, GetUploadParams)
{
    DeclareNonMutating();

    bool fetchLastKey = request->fetch_last_key();

    context->SetRequestInfo("FetchLastKey: %v", fetchLastKey);

    ValidateNotExternal();
    ValidateInUpdate();

    auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    auto* snapshotChunkList = node->GetSnapshotChunkList();
    auto* deltaChunkList = node->GetDeltaChunkList();

    const auto& uploadChunkListId = deltaChunkList->GetId();
    ToProto(response->mutable_chunk_list_id(), uploadChunkListId);

    if (fetchLastKey) {
        TOwningKey lastKey;
        if (!IsEmpty(snapshotChunkList)) {
            lastKey = GetMaxKey(snapshotChunkList);
        }
        ToProto(response->mutable_last_key(), lastKey);
    }

    context->SetResponseInfo("UploadChunkListId: %v, HasLastKey: %v",
        uploadChunkListId,
        response->has_last_key());
    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TChunkOwnerNodeProxy, EndUpload)
{
    DeclareMutating();
    ValidateTransaction();
    ValidateInUpdate();

    auto keyColumns = FromProto<TKeyColumns>(request->key_columns());
    const auto* statistics = request->has_statistics() ? &request->statistics() : nullptr;
    bool deriveStatistics = request->derive_statistics();
    bool chunkPropertiesUpdateNeeded = request->chunk_properties_update_needed();

    context->SetRequestInfo("KeyColumns: %v, ChunkPropertiesUpdateNeeded: %v",
        keyColumns,
        chunkPropertiesUpdateNeeded);

    auto* node = GetThisTypedImpl<TChunkOwnerBase>();
    YCHECK(node->GetTransaction() == Transaction);

    if (node->IsExternal()) {
        if (deriveStatistics) {
            THROW_ERROR_EXCEPTION("Cannot derive data statistics for external node");
        }
        PostToMaster(context, node->GetExternalCellTag());
    }

    node->EndUpload(statistics, deriveStatistics, keyColumns);

    node->SetChunkPropertiesUpdateNeeded(chunkPropertiesUpdateNeeded);

    SetModified();

    if (Bootstrap_->IsPrimaryMaster()) {
        auto transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->CommitTransaction(Transaction, NullTimestamp);
    }

    context->Reply();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
