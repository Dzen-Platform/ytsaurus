#include "table_reader.h"
#include "private.h"
#include "transaction.h"
#include "native_connection.h"

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/data_source.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/multi_reader_base.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_chunk_reader.h>
#include <yt/ytlib/table_client/table_ypath_proxy.h>
#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/blob_table_writer.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_listener.h>

#include <yt/ytlib/ypath/rich.h>

#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/throughput_throttler.h>
#include <yt/core/concurrency/async_stream.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/range.h>

#include <yt/core/rpc/public.h>

#include <yt/core/ytree/ypath_proxy.h>

namespace NYT {
namespace NApi {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NTableClient;
using namespace NApi;
using namespace NTransactionClient;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;
using namespace NRpc;

using NChunkClient::TDataSliceDescriptor;
using NYT::TRange;

////////////////////////////////////////////////////////////////////////////////

class TSchemalessTableReader
    : public ISchemalessMultiChunkReader
    , public TTransactionListener
{
public:
    TSchemalessTableReader(
        TTableReaderConfigPtr config,
        // TODO(ignat): Unused?
        TRemoteReaderOptionsPtr options,
        INativeClientPtr client,
        ITransactionPtr transaction,
        const TRichYPath& richPath,
        bool unordered);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;
    virtual TFuture<void> GetReadyEvent() override;

    virtual i64 GetTableRowIndex() const override;
    virtual const TNameTablePtr& GetNameTable() const override;
    virtual i64 GetTotalRowCount() const override;

    virtual TKeyColumns GetKeyColumns() const override;

    // not actually used
    virtual i64 GetSessionRowIndex() const override;
    virtual bool IsFetchingCompleted() const override;
    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;
    virtual std::vector<TChunkId> GetFailedChunkIds() const override;
    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(
        const TRange<TUnversionedRow>& unreadRows) const override;
    virtual void Interrupt() override;

private:
    const TTableReaderConfigPtr Config_;
    const TRemoteReaderOptionsPtr Options_;
    const INativeClientPtr Client_;
    const ITransactionPtr Transaction_;
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
    INativeClientPtr client,
    ITransactionPtr transaction,
    const TRichYPath& richPath,
    bool unordered)
    : Config_(CloneYsonSerializable(config))
    , Options_(options)
    , Client_(client)
    , Transaction_(transaction)
    , RichPath_(richPath)
    , TransactionId_(transaction ? transaction->GetId() : NullTransactionId)
    , Unordered_(unordered)
{
    YCHECK(Config_);
    YCHECK(Client_);

    Config_->WorkloadDescriptor.Annotations.push_back(Format("TablePath: %v", RichPath_.GetPath()));

    Logger.AddTag("Path: %v, TransactionId: %v",
        RichPath_.GetPath(),
        TransactionId_);

    ReadyEvent_ = BIND(&TSchemalessTableReader::DoOpen, MakeStrong(this))
        .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
        .Run();
}

void TSchemalessTableReader::DoOpen()
{
    const auto& path = RichPath_.GetPath();

    LOG_INFO("Opening table reader");

    TUserObject userObject;
    userObject.Path = path;

    GetUserObjectBasicAttributes(
        Client_,
        TMutableRange<TUserObject>(&userObject, 1),
        Transaction_ ? Transaction_->GetId() : NullTransactionId,
        Logger,
        EPermission::Read,
        Config_->SuppressAccessTracking);

    const auto& objectId = userObject.ObjectId;
    const auto tableCellTag = userObject.CellTag;

    auto objectIdPath = FromObjectId(objectId);

    if (userObject.Type != EObjectType::Table) {
        THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
            path,
            EObjectType::Table,
            userObject.Type);
    }

    int chunkCount;
    bool dynamic;
    TTableSchema schema;
    auto timestamp = RichPath_.GetTimestamp();

    {
        LOG_INFO("Requesting table schema");

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);

        auto req = TYPathProxy::Get(objectIdPath + "/@");
        SetTransactionId(req, Transaction_);
        SetSuppressAccessTracking(req, Config_->SuppressAccessTracking);
        std::vector<TString> attributeKeys{
            "chunk_count",
            "dynamic",
            "schema"
        };
        ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);

        auto rspOrError = WaitFor(proxy.Execute(req));
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting table schema %v",
            path);

        const auto& rsp = rspOrError.Value();
        auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

        chunkCount = attributes->Get<int>("chunk_count");
        dynamic = attributes->Get<bool>("dynamic");
        schema = attributes->Get<TTableSchema>("schema");

        if (timestamp && !(dynamic && schema.IsSorted())) {
            THROW_ERROR_EXCEPTION("Invalid attribute %Qv: table %Qv is not sorted dynamic",
                "timestamp",
                path);
        }
    }

    auto nodeDirectory = New<TNodeDirectory>();
    std::vector<TChunkSpec> chunkSpecs;

    {
        LOG_INFO("Fetching table chunks");

        FetchChunkSpecs(
            Client_,
            nodeDirectory,
            tableCellTag,
            RichPath_,
            objectId,
            RichPath_.GetRanges(),
            chunkCount,
            Config_->MaxChunksPerFetch,
            Config_->MaxChunksPerLocateRequest,
            [&] (TChunkOwnerYPathProxy::TReqFetchPtr req) {
                req->set_fetch_all_meta_extensions(false);
                req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                req->add_extension_tags(TProtoExtensionTag<NTableClient::NProto::TBoundaryKeysExt>::Value);
                SetTransactionId(req, Transaction_);
                SetSuppressAccessTracking(req, Config_->SuppressAccessTracking);
            },
            Logger,
            &chunkSpecs);

        RemoveUnavailableChunks(&chunkSpecs);
    }

    auto options = New<NTableClient::TTableReaderOptions>();
    options->EnableTableIndex = true;
    options->EnableRangeIndex = true;
    options->EnableRowIndex = true;

    auto dataSourceDirectory = New<NChunkClient::TDataSourceDirectory>();
    if (dynamic && schema.IsSorted()) {
        dataSourceDirectory->DataSources().push_back(MakeVersionedDataSource(
            path,
            schema,
            RichPath_.GetColumns(),
            timestamp.Get(AsyncLastCommittedTimestamp)));

        auto dataSliceDescriptor = TDataSliceDescriptor(std::move(chunkSpecs));

        UnderlyingReader_ = CreateSchemalessMergingMultiChunkReader(
            Config_,
            options,
            Client_,
            // HTTP proxy doesn't have a node descriptor.
            TNodeDescriptor(),
            Client_->GetNativeConnection()->GetBlockCache(),
            nodeDirectory,
            dataSourceDirectory,
            dataSliceDescriptor,
            New<TNameTable>(),
            TColumnFilter());
    } else {
        dataSourceDirectory->DataSources().push_back(MakeUnversionedDataSource(
            path,
            schema,
            RichPath_.GetColumns()));

        std::vector<TDataSliceDescriptor> dataSliceDescriptors;
        for (auto& chunkSpec : chunkSpecs) {
            dataSliceDescriptors.push_back(TDataSliceDescriptor(chunkSpec));
        }

        auto factory = Unordered_
            ? CreateSchemalessParallelMultiReader
            : CreateSchemalessSequentialMultiReader;
        UnderlyingReader_ = factory(
            Config_,
            options,
            Client_,
            // HTTP proxy doesn't have a node descriptor.
            TNodeDescriptor(),
            Client_->GetNativeConnection()->GetBlockCache(),
            nodeDirectory,
            dataSourceDirectory,
            std::move(dataSliceDescriptors),
            New<TNameTable>(),
            TColumnFilter(),
            schema.GetKeyColumns(),
            Null,
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
    rows->clear();

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
    if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
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

const TNameTablePtr& TSchemalessTableReader::GetNameTable() const
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

std::vector<TDataSliceDescriptor> TSchemalessTableReader::GetUnreadDataSliceDescriptors(
    const TRange<TUnversionedRow>& unreadRows) const
{
    Y_UNREACHABLE();
}

void TSchemalessTableReader::Interrupt()
{
    Y_UNREACHABLE();
}

void TSchemalessTableReader::RemoveUnavailableChunks(std::vector<TChunkSpec>* chunkSpecs) const
{
    std::vector<TChunkSpec> availableChunkSpecs;

    for (auto& chunkSpec : *chunkSpecs) {
        if (IsUnavailable(chunkSpec)) {
            if (!Config_->IgnoreUnavailableChunks) {
                THROW_ERROR_EXCEPTION(
                    NChunkClient::EErrorCode::ChunkUnavailable,
                    "Chunk %v is unavailable",
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
    INativeClientPtr client,
    const NYPath::TRichYPath& path,
    const TTableReaderOptions& options)
{
    ITransactionPtr transaction;

    if (options.TransactionId != NTransactionClient::NullTransactionId) {
        TTransactionAttachOptions transactionOptions;
        transactionOptions.Ping = options.Ping;
        transactionOptions.PingAncestors = options.PingAncestors;
        transaction = client->AttachTransaction(options.TransactionId, transactionOptions);
    }

    auto reader = New<TSchemalessTableReader>(
        options.Config ? options.Config : New<TTableReaderConfig>(),
        New<TRemoteReaderOptions>(),
        client,
        transaction,
        path,
        options.Unordered);

    return reader->GetReadyEvent().Apply(BIND([=] () -> ISchemalessMultiChunkReaderPtr {
        return reader;
    }));
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EColumnType,
    ((PartIndex) (0))
    ((Data) (1))
);

class TBlobTableReader
    : public IAsyncZeroCopyInputStream
{
public:
    TBlobTableReader(
        ISchemalessMultiChunkReaderPtr reader,
        const TNullable<TString>& partIndexColumnName,
        const TNullable<TString>& dataColumnName)
        : Reader_(std::move(reader))
        , PartIndexColumnName_(partIndexColumnName ? *partIndexColumnName : TBlobTableSchema::PartIndexColumn)
        , DataColumnName_(dataColumnName ? *dataColumnName : TBlobTableSchema::DataColumn)
    {
        Rows_.reserve(1);
        ColumnIndex_[EColumnType::PartIndex] = Reader_->GetNameTable()->GetIdOrRegisterName(PartIndexColumnName_);
        ColumnIndex_[EColumnType::Data] = Reader_->GetNameTable()->GetIdOrRegisterName(DataColumnName_);
    }

    virtual TFuture<TSharedRef> Read() override
    {
        if (Index_ == Rows_.size()) {
            Index_ = 0;
            bool result = Reader_->Read(&Rows_);
            if (result && Rows_.empty()) {
                return Reader_->GetReadyEvent().Apply(BIND([this, this_ = MakeStrong(this)] () {
                    Reader_->Read(&Rows_);
                    return ProcessRow();
                }));
            }
        }
        return MakeFuture(ProcessRow());
    }

private:
    const ISchemalessMultiChunkReaderPtr Reader_;
    const TString PartIndexColumnName_;
    const TString DataColumnName_;

    std::vector<TUnversionedRow> Rows_;
    size_t Index_ = 0;
    TNullable<size_t> PreviousPartIndex_;

    TEnumIndexedVector<TNullable<size_t>, EColumnType> ColumnIndex_;

    TSharedRef ProcessRow()
    {
        if (Rows_.empty()) {
            return TSharedRef();
        }

        auto row = Rows_[Index_++];
        auto value = GetDataAndValidateRow(row);

        auto holder = MakeHolder(Reader_);
        return TSharedRef(value.Data.String, value.Length, std::move(holder));
    }

    TUnversionedValue GetAndValidateValue(
        TUnversionedRow row,
        const TString& name,
        EColumnType columnType,
        EValueType expectedType)
    {
        auto columnIndex = ColumnIndex_[columnType];
        if (!columnIndex) {
            THROW_ERROR_EXCEPTION("Column %Qv not found", name);
        }

        TUnversionedValue columnValue;
        bool found = false;
        // NB: It is impossible to determine column index fast in schemaless reader.
        for (const auto& value : row) {
            if (value.Id == *columnIndex) {
                columnValue = value;
                found = true;
                break;
            }
        }

        if (!found) {
            THROW_ERROR_EXCEPTION("Column %Qv not found", name);
        }

        if (columnValue.Type != expectedType) {
            THROW_ERROR_EXCEPTION("Column %Qv must be of type %Qlv but has type %Qlv",
                name,
                expectedType,
                columnValue.Type);
        }

        return columnValue;
    }

    TUnversionedValue GetDataAndValidateRow(TUnversionedRow row)
    {
        auto partIndexValue = GetAndValidateValue(row, PartIndexColumnName_, EColumnType::PartIndex, EValueType::Int64);
        auto partIndex = partIndexValue.Data.Int64;
        if (PreviousPartIndex_) {
            if (partIndex != *PreviousPartIndex_ + 1) {
                THROW_ERROR_EXCEPTION("Values of column %Qv must be consecutive but values %v and %v violate this property",
                    PartIndexColumnName_,
                    *PreviousPartIndex_,
                    partIndex);
            }
        } else {
            if (partIndex != 0) {
                THROW_ERROR_EXCEPTION("Value of column %Qv expected to be 0, but found %v",
                    PartIndexColumnName_,
                    partIndex);
            }
        }

        PreviousPartIndex_ = partIndex;

        return GetAndValidateValue(row, DataColumnName_, EColumnType::Data, EValueType::String);
    }
};

IAsyncZeroCopyInputStreamPtr CreateBlobTableReader(
    ISchemalessMultiChunkReaderPtr reader,
    const TNullable<TString>& partIndexColumnName,
    const TNullable<TString>& dataColumnName)
{
    return New<TBlobTableReader>(
        std::move(reader),
        partIndexColumnName,
        dataColumnName);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

