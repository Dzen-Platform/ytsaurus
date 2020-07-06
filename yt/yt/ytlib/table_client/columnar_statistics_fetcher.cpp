#include "columnar_statistics_fetcher.h"

#include <yt/ytlib/chunk_client/input_chunk.h>
#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/table_client/helpers.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/client/table_client/name_table.h>

#include <yt/core/misc/protobuf_helpers.h>

namespace NYT::NTableClient {

using namespace NChunkClient;
using namespace NApi;
using namespace NNodeTrackerClient;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

TColumnarStatisticsFetcher::TColumnarStatisticsFetcher(
    TFetcherConfigPtr config,
    TNodeDirectoryPtr nodeDirectory,
    IInvokerPtr invoker,
    IFetcherChunkScraperPtr chunkScraper,
    NNative::IClientPtr client,
    EColumnarStatisticsFetcherMode mode,
    bool storeChunkStatistics,
    const TLogger& logger)
    : TFetcherBase(
        config,
        nodeDirectory,
        invoker,
        chunkScraper,
        client,
        logger)
    , Mode_(mode)
    , StoreChunkStatistics_(storeChunkStatistics)
    , ColumnFilterDictionary_(/*sortColumns=*/false)
{ }

TFuture<void> TColumnarStatisticsFetcher::FetchFromNode(
    TNodeId nodeId,
    std::vector<int> chunkIndexes)
{
    return BIND(&TColumnarStatisticsFetcher::DoFetchFromNode, MakeStrong(this), nodeId, Passed(std::move(chunkIndexes)))
        .AsyncVia(Invoker_)
        .Run();
}

TFuture<void> TColumnarStatisticsFetcher::DoFetchFromNode(TNodeId nodeId, std::vector<int> chunkIndexes)
{
    TDataNodeServiceProxy proxy(GetNodeChannel(nodeId));
    proxy.SetDefaultTimeout(Config_->NodeRpcTimeout);

    // Use nametable to replace all column names with their ids across the whole rpc request message.
    TNameTablePtr nameTable = New<TNameTable>();

    auto req = proxy.GetColumnarStatistics();
    ToProto(req->mutable_workload_descriptor(), TWorkloadDescriptor(EWorkloadCategory::UserBatch));

    for (int chunkIndex : chunkIndexes) {
        auto* subrequest = req->add_subrequests();
        for (const auto& columnName : GetColumnNames(chunkIndex)) {
            auto columnId = nameTable->GetIdOrRegisterName(columnName);
            subrequest->add_column_ids(columnId);
        }

        auto chunkId = EncodeChunkId(Chunks_[chunkIndex], nodeId);
        ToProto(subrequest->mutable_chunk_id(), chunkId);
    }

    ToProto(req->mutable_name_table(), nameTable);

    return req->Invoke().Apply(
        BIND(&TColumnarStatisticsFetcher::OnResponse, MakeStrong(this), nodeId, Passed(std::move(chunkIndexes)))
            .AsyncVia(Invoker_));
}

void TColumnarStatisticsFetcher::OnResponse(
    TNodeId nodeId,
    const std::vector<int>& chunkIndexes,
    const TDataNodeServiceProxy::TErrorOrRspGetColumnarStatisticsPtr& rspOrError)
{
    if (!rspOrError.IsOK()) {
        YT_LOG_INFO(rspOrError, "Failed to get columnar statistics from node (Address: %v, NodeId: %v)",
            NodeDirectory_->GetDescriptor(nodeId).GetDefaultAddress(),
            nodeId);
        OnNodeFailed(nodeId, chunkIndexes);
        return;
    }

    const auto& rsp = rspOrError.Value();

    for (int index = 0; index < chunkIndexes.size(); ++index) {
        int chunkIndex = chunkIndexes[index];
        const auto& subresponse = rsp->subresponses(index);
        TColumnarStatistics statistics;
        if (subresponse.has_error()) {
            auto error = NYT::FromProto<TError>(subresponse.error());
            if (error.FindMatching(NChunkClient::EErrorCode::MissingExtension)) {
                // This is an old chunk. Process it somehow.
                statistics = TColumnarStatistics::MakeEmpty(GetColumnNames(chunkIndex).size());
                statistics.LegacyChunkDataWeight = Chunks_[chunkIndex]->GetDataWeight();
            } else {
                OnChunkFailed(nodeId, chunkIndex, error);
            }
        } else {
            statistics.ColumnDataWeights = NYT::FromProto<std::vector<i64>>(subresponse.data_weights());
            YT_VERIFY(statistics.ColumnDataWeights.size() == GetColumnNames(chunkIndex).size());
            if (subresponse.has_timestamp_total_weight()) {
                statistics.TimestampTotalWeight = subresponse.timestamp_total_weight();
            }
        }
        if (StoreChunkStatistics_) {
            ChunkStatistics_[chunkIndex] = statistics;
        } else {
            LightweightChunkStatistics_[chunkIndex] = statistics.MakeLightweightStatistics();
        }
    }
}

const std::vector<TColumnarStatistics>& TColumnarStatisticsFetcher::GetChunkStatistics() const
{
    YT_VERIFY(StoreChunkStatistics_);

    return ChunkStatistics_;
}

void TColumnarStatisticsFetcher::ApplyColumnSelectivityFactors() const
{
    for (int index = 0; index < Chunks_.size(); ++index) {
        const auto& chunk = Chunks_[index];
        TLightweightColumnarStatistics statistics;
        if (StoreChunkStatistics_) {
            statistics = ChunkStatistics_[index].MakeLightweightStatistics();
        } else {
            statistics = LightweightChunkStatistics_[index];
        }
        if (statistics.LegacyChunkDataWeight == 0) {
            // We have columnar statistics, so we can adjust input chunk data weight by setting column selectivity factor.
            i64 totalColumnDataWeight = 0;
            if (chunk->GetTableChunkFormat() == ETableChunkFormat::SchemalessHorizontal ||
                chunk->GetTableChunkFormat() == ETableChunkFormat::UnversionedColumnar)
            {
                // NB: we should add total row count to the column data weights because otherwise for the empty column list
                // there will be zero data weight which does not allow unordered pool to work properly.
                totalColumnDataWeight += chunk->GetTotalRowCount();
            } else if (
                chunk->GetTableChunkFormat() == ETableChunkFormat::VersionedSimple ||
                chunk->GetTableChunkFormat() == ETableChunkFormat::VersionedColumnar)
            {
                // Default value of sizeof(TTimestamp) = 8 is used for versioned chunks that were written before
                // we started to save the timestamp statistics to columnar statistics extension.
                totalColumnDataWeight += statistics.TimestampTotalWeight.value_or(sizeof(TTimestamp));
            } else {
                THROW_ERROR_EXCEPTION("Cannot apply column selectivity factor for chunk of an old table format")
                    << TErrorAttribute("chunk_id", chunk->ChunkId())
                    << TErrorAttribute("table_chunk_format", chunk->GetTableChunkFormat());
            }
            totalColumnDataWeight += statistics.ColumnDataWeightsSum;
            auto totalDataWeight = chunk->GetTotalDataWeight();
            chunk->SetColumnSelectivityFactor(std::min(static_cast<double>(totalColumnDataWeight) / totalDataWeight, 1.0));
        }
    }
}

TFuture<void> TColumnarStatisticsFetcher::Fetch()
{
    if (StoreChunkStatistics_) {
        ChunkStatistics_.resize(Chunks_.size());
        for (int chunkIndex = 0; chunkIndex < Chunks_.size(); ++chunkIndex ) {
            if (Chunks_[chunkIndex ]->IsDynamicStore()) {
                ChunkStatistics_[chunkIndex] = TColumnarStatistics::MakeEmpty(GetColumnNames(chunkIndex).size());
            }
        }
    } else {
        LightweightChunkStatistics_.resize(Chunks_.size());
    }

    if (Mode_ == EColumnarStatisticsFetcherMode::FromMaster) {
        return VoidFuture;
    }

    return TFetcherBase::Fetch();
}

void TColumnarStatisticsFetcher::AddChunk(TInputChunkPtr chunk, std::vector<TString> columnNames)
{
    if (columnNames.empty() || chunk->IsDynamicStore()) {
        // Do not fetch anything. The less rpc requests, the better.
        Chunks_.emplace_back(std::move(chunk));
    } else {
        const NProto::THeavyColumnStatisticsExt* heavyColumnStatistics = nullptr;
        if (Mode_ == EColumnarStatisticsFetcherMode::FromMaster || Mode_ == EColumnarStatisticsFetcherMode::Fallback) {
            heavyColumnStatistics = chunk->HeavyColumnarStatisticsExt().get();
        }
        if (heavyColumnStatistics || Mode_ == EColumnarStatisticsFetcherMode::FromMaster) {
            TColumnarStatistics columnarStatistics;
            if (heavyColumnStatistics) {
                columnarStatistics = GetColumnarStatistics(*heavyColumnStatistics, columnNames);
            } else {
                YT_VERIFY(Mode_ == EColumnarStatisticsFetcherMode::FromMaster);
                columnarStatistics = TColumnarStatistics::MakeEmpty(columnNames.size());
                columnarStatistics.LegacyChunkDataWeight = chunk->GetDataWeight();
            }
            Chunks_.emplace_back(std::move(chunk));
            if (StoreChunkStatistics_) {
                ChunkStatistics_.resize(Chunks_.size());
                ChunkStatistics_.back() = columnarStatistics;
            } else {
                LightweightChunkStatistics_.resize(Chunks_.size());
                LightweightChunkStatistics_.back() = columnarStatistics.MakeLightweightStatistics();
            }
        } else {
            TFetcherBase::AddChunk(std::move(chunk));
        }
    }

    ChunkColumnFilterIds_.emplace_back(ColumnFilterDictionary_.GetIdOrRegisterAdmittedColumns(columnNames));
}

const std::vector<TString>& TColumnarStatisticsFetcher::GetColumnNames(int chunkIndex) const
{
    return ColumnFilterDictionary_.GetAdmittedColumns(ChunkColumnFilterIds_[chunkIndex]);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
