#include "stdafx.h"
#include "table_reader.h"
#include "private.h"

#include <ytlib/chunk_client/chunk_spec.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/multi_chunk_reader_base.h>
#include <ytlib/chunk_client/helpers.h>

#include <ytlib/cypress_client/rpc_helpers.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/helpers.h>

#include <ytlib/transaction_client/helpers.h>
#include <ytlib/transaction_client/transaction_listener.h>
#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/table_client/schemaless_chunk_reader.h>
#include <ytlib/table_client/table_ypath_proxy.h>
#include <ytlib/table_client/name_table.h>
#include <ytlib/table_client/chunk_meta_extensions.h>

#include <ytlib/ypath/rich.h>

#include <core/concurrency/scheduler.h>
#include <core/concurrency/throughput_throttler.h>

#include <core/misc/protobuf_helpers.h>

#include <core/ytree/ypath_proxy.h>

#include <core/rpc/public.h>

namespace NYT {
namespace NApi {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NTransactionClient;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TSchemalessTableReader
    : public ISchemalessMultiChunkReader
    , public TTransactionListener
{
public:
    TSchemalessTableReader(
        TTableReaderConfigPtr config,
        TRemoteReaderOptionsPtr options,
        IClientPtr client,
        TTransactionPtr transaction,
        const TRichYPath& richPath,
        bool unordered);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;
    virtual TFuture<void> GetReadyEvent() override;

    virtual i64 GetTableRowIndex() const override;
    virtual TNameTablePtr GetNameTable() const override;
    virtual i64 GetTotalRowCount() const override;

    virtual TKeyColumns GetKeyColumns() const override;

    // not actually used
    virtual i64 GetSessionRowIndex() const override;
    virtual bool IsFetchingCompleted() const override;
    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;
    virtual std::vector<TChunkId> GetFailedChunkIds() const override;

private:
    const TTableReaderConfigPtr Config_;
    const TRemoteReaderOptionsPtr Options_;
    const IClientPtr Client_;
    const TTransactionPtr Transaction_;
    const TRichYPath RichPath_;

    const TTransactionId TransactionId_;
    const bool Unordered_;

    TFuture<void> ReadyEvent_;

    ISchemalessMultiChunkReaderPtr UnderlyingReader_;

    NLogging::TLogger Logger = ApiLogger;

    void DoOpen();
    void RemoveUnavailableChunks(std::vector<TChunkSpec>* chunkSpecs) const;
};

////////////////////////////////////////////////////////////////////////////////

TSchemalessTableReader::TSchemalessTableReader(
    TTableReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    IClientPtr client,
    TTransactionPtr transaction,
    const TRichYPath& richPath,
    bool unordered)
    : Config_(config)
    , Options_(options)
    , Client_(client)
    , Transaction_(transaction)
    , RichPath_(richPath)
    , TransactionId_(transaction ? transaction->GetId() : NullTransactionId)
    , Unordered_(unordered)
{
    YCHECK(Config_);
    YCHECK(Client_);

    Logger.AddTag("Path: %v, TransactionId: %v",
        RichPath_.GetPath(),
        TransactionId_);

    ReadyEvent_ = BIND(&TSchemalessTableReader::DoOpen, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run();
}

void TSchemalessTableReader::DoOpen()
{
    const auto& path = RichPath_.GetPath();

    LOG_INFO("Opening table reader");

    auto tableCellTag = InvalidCellTag;
    TObjectId objectId;

    {
        LOG_INFO("Requesting basic attributes");

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
        TObjectServiceProxy proxy(channel);

        auto req = TTableYPathProxy::GetBasicAttributes(path);
        req->set_permissions(static_cast<ui32>(EPermission::Read));
        SetTransactionId(req, Transaction_);
        SetSuppressAccessTracking(req, Config_->SuppressAccessTracking);

        auto rspOrError = WaitFor(proxy.Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting basic attributes for table %v",
            path);

        const auto& rsp = rspOrError.Value();

        objectId = FromProto<TObjectId>(rsp->object_id());
        tableCellTag = rsp->cell_tag();

        LOG_INFO("Basic attributes received (ObjectId: %v, CellTag: %v)",
            objectId,
            tableCellTag);
    }

    auto objectIdPath = FromObjectId(objectId);

    {
        auto type = TypeFromId(objectId);
        if (type != EObjectType::Table) {
            THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
                path,
                EObjectType::Table,
                type);
        }
    }

    bool dynamic;
    TTableSchema schema;
    TKeyColumns keyColumns;

    {
        LOG_INFO("Requesting table schema");

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
        TObjectServiceProxy proxy(channel);

        auto req = TYPathProxy::Get(objectIdPath);
        SetTransactionId(req, Transaction_);
        SetSuppressAccessTracking(req, Config_->SuppressAccessTracking);
        TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
        attributeFilter.Keys.push_back("dynamic");
        attributeFilter.Keys.push_back("schema");
        attributeFilter.Keys.push_back("key_columns");
        ToProto(req->mutable_attribute_filter(), attributeFilter);

        auto rspOrError = WaitFor(proxy.Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting table schema %v",
            path);

        const auto& rsp = rspOrError.Value();
        auto node = ConvertToNode(TYsonString(rsp->value()));
        const auto& attributes = node->Attributes();

        dynamic = attributes.Get<bool>("dynamic");

        if (dynamic) {
            schema = attributes.Get<TTableSchema>("schema");
            keyColumns = attributes.Get<TKeyColumns>("key_columns");
        }
    }

    auto nodeDirectory = New<TNodeDirectory>();
    std::vector<TChunkSpec> chunkSpecs;

    {
        LOG_INFO("Fetching table chunks");

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower, tableCellTag);
        TObjectServiceProxy proxy(channel);

        auto req = TTableYPathProxy::Fetch(objectIdPath);
        InitializeFetchRequest(req.Get(), RichPath_);
        req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
        req->add_extension_tags(TProtoExtensionTag<NTableClient::NProto::TBoundaryKeysExt>::Value);
        SetTransactionId(req, Transaction_);
        SetSuppressAccessTracking(req, Config_->SuppressAccessTracking);

        auto rspOrError = WaitFor(proxy.Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error fetching chunks for table %v",
            path);
        const auto& rsp = rspOrError.Value();

        ProcessFetchResponse(
            Client_,
            rsp,
            tableCellTag,
            nodeDirectory,
            Config_->MaxChunksPerLocateRequest,
            Logger,
            &chunkSpecs);

        RemoveUnavailableChunks(&chunkSpecs);
    }

    auto options = New<NTableClient::TTableReaderOptions>();
    options->EnableTableIndex = true;
    options->EnableRangeIndex = true;
    options->EnableRowIndex = true;

    if (dynamic) {
        UnderlyingReader_ = CreateSchemalessMergingMultiChunkReader(
            Config_,
            options,
            Client_,
            Client_->GetConnection()->GetBlockCache(),
            nodeDirectory,
            std::move(chunkSpecs),
            New<TNameTable>(),
            TColumnFilter(),
            schema,
            keyColumns);
    } else {
        auto factory = Unordered_
            ? CreateSchemalessParallelMultiChunkReader
            : CreateSchemalessSequentialMultiChunkReader;
        UnderlyingReader_ = factory(
            Config_,
            options,
            Client_,
            Client_->GetConnection()->GetBlockCache(),
            nodeDirectory,
            std::move(chunkSpecs),
            New<TNameTable>(),
            TColumnFilter(),
            TKeyColumns(),
            NConcurrency::GetUnlimitedThrottler());
    }

    WaitFor(UnderlyingReader_->GetReadyEvent())
        .ThrowOnError();

    if (Transaction_) {
        ListenTransaction(Transaction_);
    }

    LOG_INFO("Table reader opened");
}

bool TSchemalessTableReader::Read(std::vector<TUnversionedRow> *rows)
{
    if (IsAborted()) {
        return true;
    }

    if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
        return true;
    }

    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->Read(rows);
}

TFuture<void> TSchemalessTableReader::GetReadyEvent()
{
    if (!ReadyEvent_.IsSet()) {
        return ReadyEvent_;
    }

    if (IsAborted()) {
        return MakeFuture(TError("Transaction %v aborted",
            TransactionId_));
    }

    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetReadyEvent();
}

i64 TSchemalessTableReader::GetTableRowIndex() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetTableRowIndex();
}

i64 TSchemalessTableReader::GetTotalRowCount() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetTotalRowCount();
}

TNameTablePtr TSchemalessTableReader::GetNameTable() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetNameTable();
}

TKeyColumns TSchemalessTableReader::GetKeyColumns() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetKeyColumns();
}

i64 TSchemalessTableReader::GetSessionRowIndex() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetSessionRowIndex();
}

bool TSchemalessTableReader::IsFetchingCompleted() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->IsFetchingCompleted();
}

NChunkClient::NProto::TDataStatistics TSchemalessTableReader::GetDataStatistics() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetDataStatistics();
}

std::vector<TChunkId> TSchemalessTableReader::GetFailedChunkIds() const
{
    YCHECK(UnderlyingReader_);
    return UnderlyingReader_->GetFailedChunkIds();
}

void TSchemalessTableReader::RemoveUnavailableChunks(std::vector<TChunkSpec>* chunkSpecs) const
{
    std::vector<TChunkSpec> availableChunkSpecs;

    for (auto& chunkSpec : *chunkSpecs) {
        if (IsUnavailable(chunkSpec)) {
            if (!Config_->IgnoreUnavailableChunks) {
                THROW_ERROR_EXCEPTION("Chunk %v is unavailable",
                    NYT::FromProto<TChunkId>(chunkSpec.chunk_id()));
            }
        } else {
            availableChunkSpecs.push_back(std::move(chunkSpec));
        }
    }

    *chunkSpecs = std::move(availableChunkSpecs);
}

////////////////////////////////////////////////////////////////////////////////

TFuture<ISchemalessMultiChunkReaderPtr> CreateTableReader(
    IClientPtr client,
    const NYPath::TRichYPath& path,
    const TTableReaderOptions& options)
{
    NTransactionClient::TTransactionPtr transaction;

    if (options.TransactionId != NTransactionClient::NullTransactionId) {
        NTransactionClient::TTransactionAttachOptions transactionOptions;
        transactionOptions.Ping = options.Ping;
        transactionOptions.PingAncestors = options.PingAncestors;

        transaction = client->GetTransactionManager()->Attach(
            options.TransactionId,
            transactionOptions);
    }

    ISchemalessMultiChunkReaderPtr reader = New<TSchemalessTableReader>(
        options.Config ? options.Config : New<TTableReaderConfig>(),
        New<TRemoteReaderOptions>(),
        client,
        transaction,
        path,
        options.Unordered);

    return reader->GetReadyEvent().Apply(BIND([=] () {
        return reader;
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

