#include "journal_reader.h"
#include "private.h"
#include "config.h"
#include "connection.h"

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/read_limit.h>
#include <yt/ytlib/chunk_client/replication_reader.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/journal_client/journal_ypath_proxy.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_listener.h>
#include <yt/ytlib/transaction_client/transaction_manager.h>

#include <yt/core/logging/log.h>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NYPath;
using namespace NYTree;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NJournalClient;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

class TJournalReader
    : public TTransactionListener
    , public IJournalReader
{
public:
    TJournalReader(
        IClientPtr client,
        const TYPath& path,
        const TJournalReaderOptions& options)
        : Client_(client)
        , Path_(path)
        , Options_(options)
        , Config_(options.Config ? options.Config : New<TJournalReaderConfig>())
        , Logger(ApiLogger)
    {
        if (Options_.TransactionId) {
            auto transactionManager = Client_->GetTransactionManager();
            Transaction_ = transactionManager->Attach(Options_.TransactionId);
        }

        Logger.AddTag("Path: %v, TransactionId: %v",
            Path_,
            Options_.TransactionId);
    }

    virtual TFuture<void> Open() override
    {
        return BIND(&TJournalReader::DoOpen, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

    virtual TFuture<std::vector<TSharedRef>> Read() override
    {
        return BIND(&TJournalReader::DoRead, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

private:
    IClientPtr Client_;
    TYPath Path_;
    TJournalReaderOptions Options_;
    TJournalReaderConfigPtr Config_;

    TTransactionPtr Transaction_;

    TNodeDirectoryPtr NodeDirectory_ = New<TNodeDirectory>();
    std::vector<NChunkClient::NProto::TChunkSpec> ChunkSpecs_;

    int CurrentChunkIndex_ = -1;
    bool Finished_ = false;
    IChunkReaderPtr CurrentChunkReader_;

    // Inside current chunk.
    i64 BeginRowIndex_ = -1;
    i64 CurrentRowIndex_ = -1;
    i64 EndRowIndex_ = -1; // exclusive

    NLogging::TLogger Logger;


    void DoOpen()
    {
        LOG_INFO("Opening journal reader");

        auto cellTag = InvalidCellTag;
        TObjectId objectId;

        {
            LOG_INFO("Requesting basic attributes");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
            TObjectServiceProxy proxy(channel);

            auto req = TJournalYPathProxy::GetBasicAttributes(Path_);
            req->set_permissions(static_cast<ui32>(EPermission::Read));
            SetTransactionId(req, Transaction_);

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting basic attributes for journal %v",
                Path_);

            const auto& rsp = rspOrError.Value();

            objectId = FromProto<TObjectId>(rsp->object_id());
            cellTag = rsp->cell_tag();

            LOG_INFO("Basic attributes received (ObjectId: %v, CellTag: %v)",
                objectId,
                cellTag);
        }

        {
            auto type = TypeFromId(objectId);
            if (type != EObjectType::Journal) {
                THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
                    Path_,
                    EObjectType::Journal,
                    type);
            }
        }

        auto objectIdPath = FromObjectId(objectId);

        {
            LOG_INFO("Fetching journal chunks");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower, cellTag);
            TObjectServiceProxy proxy(channel);

            auto req = TJournalYPathProxy::Fetch(objectIdPath);

            TReadLimit lowerLimit, upperLimit;
            i64 firstRowIndex = Options_.FirstRowIndex.Get(0);
            if (Options_.FirstRowIndex) {
                lowerLimit.SetRowIndex(firstRowIndex);
            }
            if (Options_.RowCount) {
                upperLimit.SetRowIndex(firstRowIndex + *Options_.RowCount);
            }

            TReadRange range;
            range.LowerLimit() = lowerLimit;
            range.UpperLimit() = upperLimit;
            ToProto(req->mutable_ranges(), std::vector<TReadRange>({range}));

            SetTransactionId(req, Transaction_);
            SetSuppressAccessTracking(req, Options_.SuppressAccessTracking);
            req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error fetching journals for table %v",
                Path_);
            const auto& rsp = rspOrError.Value();

            ProcessFetchResponse(
                Client_,
                rsp,
                cellTag,
                NodeDirectory_,
                std::numeric_limits<int>::max(), // no foreign chunks are possible anyway
                Logger,
                &ChunkSpecs_);
        }

        if (Transaction_) {
            ListenTransaction(Transaction_);
        }

        LOG_INFO("Journal reader opened");
    }

    std::vector<TSharedRef> DoRead()
    {
        while (true) {
            ValidateAborted();

            if (Finished_) {
                return std::vector<TSharedRef>();
            }

            if (!CurrentChunkReader_) {
                if (++CurrentChunkIndex_ >= ChunkSpecs_.size()) {
                    Finished_ = true;
                    return std::vector<TSharedRef>();
                }

                const auto& chunkSpec = ChunkSpecs_[CurrentChunkIndex_];

                auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
                auto replicas = FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());
                auto options = New<TRemoteReaderOptions>();
                CurrentChunkReader_ = CreateReplicationReader(
                    Config_,
                    options,
                    Client_,
                    NodeDirectory_,
                    Null,
                    chunkId,
                    replicas,
                    Client_->GetConnection()->GetBlockCache());

                // NB: Lower/upper limits are mandatory for journal chunks.
                auto lowerLimit = FromProto<TReadLimit>(chunkSpec.lower_limit());
                BeginRowIndex_ = lowerLimit.GetRowIndex();

                auto upperLimit = FromProto<TReadLimit>(chunkSpec.upper_limit());
                EndRowIndex_ = upperLimit.GetRowIndex();

                CurrentRowIndex_ = BeginRowIndex_;
            }

            auto rowsOrError = WaitFor(CurrentChunkReader_->ReadBlocks(
                CurrentRowIndex_,
                EndRowIndex_ - CurrentRowIndex_));
            THROW_ERROR_EXCEPTION_IF_FAILED(rowsOrError);

            const auto& rows = rowsOrError.Value();
            if (!rows.empty()) {
                CurrentRowIndex_ += rows.size();
                return rows;
            }

            CurrentChunkReader_.Reset();
        }
    }

};

IJournalReaderPtr CreateJournalReader(
    IClientPtr client,
    const TYPath& path,
    const TJournalReaderOptions& options)
{
    return New<TJournalReader>(client, path, options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
