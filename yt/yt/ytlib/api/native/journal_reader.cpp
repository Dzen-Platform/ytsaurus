#include "journal_reader.h"
#include "private.h"
#include "config.h"
#include "connection.h"
#include "transaction.h"
#include "private.h"

#include <yt/client/api/journal_reader.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/chunk_reader_statistics.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/journal_client/journal_ypath_proxy.h>
#include <yt/ytlib/journal_client/chunk_reader.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_listener.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/client/object_client/helpers.h>

#include <yt/client/chunk_client/read_limit.h>

#include <yt/core/concurrency/action_queue.h>

namespace NYT::NApi::NNative {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NJournalClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NTransactionClient;
using namespace NYPath;
using namespace NYTree;

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
        , Logger(ApiLogger.WithTag("Path: %v, TransactionId: %v",
            Path_,
            Options_.TransactionId))
        , ReaderInvoker_(CreateSerializedInvoker(NChunkClient::TDispatcher::Get()->GetReaderInvoker()))
    {
        if (Options_.TransactionId) {
            Transaction_ = Client_->AttachTransaction(Options_.TransactionId);
        }
    }

    virtual TFuture<void> Open() override
    {
        return BIND(&TJournalReader::DoOpen, MakeStrong(this))
            .AsyncVia(ReaderInvoker_)
            .Run();
    }

    virtual TFuture<std::vector<TSharedRef>> Read() override
    {
        return BIND(&TJournalReader::DoRead, MakeStrong(this))
            .AsyncVia(ReaderInvoker_)
            .Run();
    }

private:
    const IClientPtr Client_;
    const TYPath Path_;
    const TJournalReaderOptions Options_;
    const TJournalReaderConfigPtr Config_;

    const NLogging::TLogger Logger;

    NApi::ITransactionPtr Transaction_;

    const TNodeDirectoryPtr NodeDirectory_ = New<TNodeDirectory>();
    std::vector<NChunkClient::NProto::TChunkSpec> ChunkSpecs_;

    IInvokerPtr ReaderInvoker_;

    int CurrentChunkIndex_ = -1;
    bool Finished_ = false;
    IChunkReaderPtr CurrentChunkReader_;

    // Inside current chunk.
    i64 BeginRowIndex_ = -1;
    i64 CurrentRowIndex_ = -1;
    i64 EndRowIndex_ = -1; // exclusive


    void DoOpen()
    {
        YT_LOG_DEBUG("Opening journal reader");

        TUserObject userObject(Path_);

        GetUserObjectBasicAttributes(
            Client_,
            {&userObject},
            Transaction_ ? Transaction_->GetId() : NullTransactionId,
            Logger,
            EPermission::Read);

        if (userObject.Type != EObjectType::Journal) {
            THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
                Path_,
                EObjectType::Journal,
                userObject.Type);
        }

        {
            YT_LOG_DEBUG("Fetching journal chunks");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower, userObject.ExternalCellTag);
            TObjectServiceProxy proxy(channel);

            auto batchReq = proxy.ExecuteBatchWithRetries(Client_->GetNativeConnection()->GetConfig()->ChunkFetchRetries);

            auto req = TJournalYPathProxy::Fetch(userObject.GetObjectIdPath());
            AddCellTagToSyncWith(req, userObject.ObjectId);
            req->set_fetch_parity_replicas(true);

            TLegacyReadLimit lowerLimit, upperLimit;
            i64 firstRowIndex = Options_.FirstRowIndex.value_or(0);
            if (Options_.FirstRowIndex) {
                lowerLimit.SetRowIndex(firstRowIndex);
            }
            if (Options_.RowCount) {
                upperLimit.SetRowIndex(firstRowIndex + *Options_.RowCount);
            }
            ToProto(req->mutable_ranges(), std::vector<TLegacyReadRange>({{lowerLimit, upperLimit}}));

            AddCellTagToSyncWith(req, userObject.ObjectId);
            SetTransactionId(req, userObject.ExternalTransactionId);
            SetSuppressAccessTracking(req, Options_.SuppressAccessTracking);
            SetSuppressExpirationTimeoutRenewal(req, Options_.SuppressExpirationTimeoutRenewal);
            req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);

            batchReq->AddRequest(req);
            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error fetching chunks for journal %v",
                Path_);

            const auto& batchRsp = batchRspOrError.Value();
            const auto& rspOrError = batchRsp->GetResponse<TJournalYPathProxy::TRspFetch>(0);
            const auto& rsp = rspOrError.Value();

            ProcessFetchResponse(
                Client_,
                rsp,
                userObject.ExternalCellTag,
                NodeDirectory_,
                std::numeric_limits<int>::max(), // no foreign chunks are possible anyway
                std::nullopt,
                Logger,
                &ChunkSpecs_);
        }

        if (Transaction_) {
            StartListenTransaction(Transaction_);
        }

        YT_LOG_DEBUG("Journal reader opened");
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
                auto codecId = FromProto<NErasure::ECodec>(chunkSpec.erasure_codec());
                auto replicas = FromProto<TChunkReplicaList>(chunkSpec.replicas());
                CurrentChunkReader_ = NJournalClient::CreateChunkReader(
                    Config_,
                    Client_,
                    NodeDirectory_,
                    chunkId,
                    codecId,
                    replicas,
                    Client_->GetNativeConnection()->GetBlockCache());

                // NB: Lower/upper limits are mandatory for journal chunks.
                if (!chunkSpec.has_lower_limit()) {
                    THROW_ERROR_EXCEPTION("Lower limit is missing in chunk spec");
                }
                if (!chunkSpec.has_upper_limit()) {
                    THROW_ERROR_EXCEPTION("Upper limit is missing in chunk spec");
                }

                auto lowerLimit = FromProto<TLegacyReadLimit>(chunkSpec.lower_limit());
                BeginRowIndex_ = lowerLimit.GetRowIndex();

                auto upperLimit = FromProto<TLegacyReadLimit>(chunkSpec.upper_limit());
                EndRowIndex_ = upperLimit.GetRowIndex();

                CurrentRowIndex_ = BeginRowIndex_;

                YT_LOG_DEBUG("Switched to another journal chunk (ChunkId: %v, PhysicalRowIndexes: %v-%v)",
                    chunkId,
                    BeginRowIndex_,
                    EndRowIndex_ - 1);
            }

            // TODO(savrus): profile chunk reader statistics.
            TClientBlockReadOptions options;
            options.WorkloadDescriptor = Config_->WorkloadDescriptor;
            options.ChunkReaderStatistics = New<TChunkReaderStatistics>();

            auto rowsOrError = WaitFor(CurrentChunkReader_->ReadBlocks(
                options,
                CurrentRowIndex_,
                static_cast<int>(EndRowIndex_ - CurrentRowIndex_)));
            THROW_ERROR_EXCEPTION_IF_FAILED(rowsOrError);

            const auto& rowsBlocks = rowsOrError.Value();
            if (!rowsBlocks.empty()) {
                CurrentRowIndex_ += rowsBlocks.size();
                return TBlock::Unwrap(rowsBlocks);
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

} // namespace NYT::NApi::NNative
