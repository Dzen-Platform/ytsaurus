#include "file_writer.h"
#include "private.h"
#include "client.h"
#include "config.h"
#include "transaction.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/private.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/ytlib/file_client/file_chunk_writer.h>
#include <yt/ytlib/file_client/file_ypath_proxy.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_listener.h>
#include <yt/ytlib/transaction_client/config.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/logging/log.h>

#include <yt/core/rpc/helpers.h>

#include <yt/core/ytree/convert.h>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NRpc;
using namespace NYTree;
using namespace NYPath;
using namespace NYson;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NApi;
using namespace NTransactionClient;
using namespace NFileClient;

////////////////////////////////////////////////////////////////////////////////

class TFileWriter
    : public TTransactionListener
    , public IFileWriter
{
public:
    TFileWriter(
        INativeClientPtr client,
        const TYPath& path,
        const TFileWriterOptions& options)
        : Client_(client)
        , Path_(path)
        , Options_(options)
        , Config_(options.Config ? options.Config : New<TFileWriterConfig>())
    {
        Logger.AddTag("Path: %v, TransactionId: %v",
            Path_,
            Options_.TransactionId);
    }

    virtual TFuture<void> Open() override
    {
        return BIND(&TFileWriter::DoOpen, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

    virtual TFuture<void> Write(const TSharedRef& data) override
    {
        ValidateAborted();

        if (Writer_->Write(data)) {
            return VoidFuture;
        }

        return Writer_->GetReadyEvent();
    }

    virtual TFuture<void> Close() override
    {
        return BIND(&TFileWriter::DoClose, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

private:
    const INativeClientPtr Client_;
    const TYPath Path_;
    const TFileWriterOptions Options_;
    const TFileWriterConfigPtr Config_;

    ITransactionPtr Transaction_;
    ITransactionPtr UploadTransaction_;

    IFileMultiChunkWriterPtr Writer_;

    TCellTag CellTag_ = InvalidCellTag;
    TObjectId ObjectId_;

    NLogging::TLogger Logger = ApiLogger;


    void DoOpen()
    {
        if (Options_.TransactionId) {
            Transaction_ = Client_->AttachTransaction(Options_.TransactionId);
            ListenTransaction(Transaction_);
        }

        auto writerOptions = New<TMultiChunkWriterOptions>();

        TUserObject userObject;
        userObject.Path = Path_;

        GetUserObjectBasicAttributes(
            Client_,
            TMutableRange<TUserObject>(&userObject, 1),
            Transaction_ ? Transaction_->GetId() : NullTransactionId,
            Logger,
            EPermission::Write);

        CellTag_ = userObject.CellTag;
        ObjectId_ = userObject.ObjectId;
        
        if (userObject.Type != EObjectType::File) {
            THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
                Path_,
                EObjectType::File,
                userObject.Type);
        }

        auto objectIdPath = FromObjectId(ObjectId_);

        {
            LOG_INFO("Requesting extended file attributes");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
            TObjectServiceProxy proxy(channel);

            auto req = TCypressYPathProxy::Get(objectIdPath + "/@");
            SetTransactionId(req, Transaction_);
            std::vector<TString> attributeKeys{
                "account",
                "compression_codec",
                "erasure_codec",
                "primary_medium",
                "replication_factor"
            };
            ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(
                rspOrError,
                "Error requesting extended attributes of file %v",
                Path_);

            auto rsp = rspOrError.Value();
            auto attributes = ConvertToAttributes(TYsonString(rsp->value()));
            writerOptions->ReplicationFactor = attributes->Get<int>("replication_factor");
            writerOptions->MediumName = attributes->Get<TString>("primary_medium");
            writerOptions->Account = attributes->Get<TString>("account");

            if (Options_.CompressionCodec) {
                writerOptions->CompressionCodec = *Options_.CompressionCodec;
            } else {
                writerOptions->CompressionCodec = attributes->Get<NCompression::ECodec>("compression_codec");
            }

            if (Options_.ErasureCodec) {
                writerOptions->ErasureCodec = *Options_.ErasureCodec;
            } else {
                writerOptions->ErasureCodec = attributes->Get<NErasure::ECodec>(
                    "erasure_codec",
                    NErasure::ECodec::None);
            }

            LOG_INFO("Extended file attributes received (Account: %v)",
                writerOptions->Account);
        }

        {
            LOG_INFO("Starting file upload");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
            TObjectServiceProxy proxy(channel);

            auto batchReq = proxy.ExecuteBatch();

            {
                auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
                for (const auto& id : Options_.PrerequisiteTransactionIds) {
                    auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                    ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
                }
            }

            {
                auto req = TFileYPathProxy::BeginUpload(objectIdPath);
                req->set_update_mode(static_cast<int>(Options_.Append ? EUpdateMode::Append : EUpdateMode::Overwrite));
                req->set_lock_mode(static_cast<int>(Options_.Append ? ELockMode::Shared : ELockMode::Exclusive));
                req->set_upload_transaction_title(Format("Upload to %v", Path_));
                req->set_upload_transaction_timeout(ToProto(Config_->UploadTransactionTimeout));
                GenerateMutationId(req);
                SetTransactionId(req, Transaction_);
                batchReq->AddRequest(req, "begin_upload");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                "Error starting upload to file %v",
                Path_);
            const auto& batchRsp = batchRspOrError.Value();

            {
                auto rsp = batchRsp->GetResponse<TFileYPathProxy::TRspBeginUpload>("begin_upload").Value();
                auto uploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());

                TTransactionAttachOptions options;
                options.PingAncestors = Options_.PingAncestors;
                options.AutoAbort = true;

                UploadTransaction_ = Client_->AttachTransaction(uploadTransactionId, options);
                ListenTransaction(UploadTransaction_);

                LOG_INFO("File upload started (UploadTransactionId: %v)",
                    uploadTransactionId);
            }
        }

        TChunkListId chunkListId;

        {
            LOG_INFO("Requesting file upload parameters");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower, CellTag_);
            TObjectServiceProxy proxy(channel);

            auto req = TFileYPathProxy::GetUploadParams(objectIdPath);
            SetTransactionId(req, UploadTransaction_);

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(
                rspOrError,
                "Error requesting upload parameters for file %v",
                Path_);

            const auto& rsp = rspOrError.Value();
            chunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());

            LOG_INFO("File upload parameters received (ChunkListId: %v)",
                chunkListId);
        }

        Writer_ = CreateFileMultiChunkWriter(
            Config_,
            writerOptions,
            Client_,
            CellTag_,
            UploadTransaction_->GetId(),
            chunkListId);

        WaitFor(Writer_->Open())
            .ThrowOnError();

        LOG_INFO("File opened");
    }

    void DoWrite(const TSharedRef& data)
    {
        ValidateAborted();

        if (!Writer_->Write(data)) {
            WaitFor(Writer_->GetReadyEvent())
                .ThrowOnError();
        }
    }

    void DoClose()
    {
        ValidateAborted();

        LOG_INFO("Closing file");

        {
            auto result = WaitFor(Writer_->Close());
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Failed to close file writer");
        }

        UploadTransaction_->Ping();
        UploadTransaction_->Detach();

        auto objectIdPath = FromObjectId(ObjectId_);

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        {
            auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
            for (const auto& id : Options_.PrerequisiteTransactionIds) {
                auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
            }
        }

        {
            auto req = TFileYPathProxy::EndUpload(objectIdPath);
            *req->mutable_statistics() = Writer_->GetDataStatistics();

            if (Options_.CompressionCodec) {
                req->set_compression_codec(static_cast<int>(*Options_.CompressionCodec));
            }

            if (Options_.ErasureCodec) {
                req->set_erasure_codec(static_cast<int>(*Options_.ErasureCodec));
            }

            SetTransactionId(req, UploadTransaction_);
            GenerateMutationId(req);
            batchReq->AddRequest(req, "end_upload");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            GetCumulativeError(batchRspOrError),
            "Error finishing upload to file %v",
            Path_);

        LOG_INFO("File closed");
    }

};

IFileWriterPtr CreateFileWriter(
    INativeClientPtr client,
    const TYPath& path,
    const TFileWriterOptions& options)
{
    return New<TFileWriter>(client, path, options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
