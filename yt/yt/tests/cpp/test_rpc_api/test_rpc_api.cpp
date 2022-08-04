//%NUM_MASTERS=1
//%NUM_NODES=3
//%NUM_SCHEDULERS=0
//%DRIVER_BACKENDS=['rpc']
//%ENABLE_RPC_PROXY=True
//%DELTA_MASTER_CONFIG={"object_service":{"timeout_backoff_lead_time":100}}

#include <yt/yt/tests/cpp/modify_rows_test.h>

#include <yt/yt/tests/cpp/test_base/api_test_base.h>

#include <yt/yt/client/api/rpc_proxy/transaction_impl.h>

#include <yt/yt/client/api/client.h>
#include <yt/yt/client/api/rowset.h>
#include <yt/yt/client/api/transaction.h>
#include <yt/yt/client/api/table_writer.h>

#include <yt/yt/client/api/rpc_proxy/helpers.h>
#include <yt/yt/client/api/rpc_proxy/public.h>

#include <yt/yt/client/object_client/public.h>
#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/table_client/helpers.h>
#include <yt/yt/client/table_client/name_table.h>

#include <yt/yt/client/transaction_client/timestamp_provider.h>

#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/ytree/convert.h>

#include <yt/yt/library/auth/tvm.h>

#include <library/cpp/tvmauth/client/mocked_updater.h>

////////////////////////////////////////////////////////////////////////////////

namespace NYT {
namespace NCppTests {
namespace {

using namespace NApi;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NSecurityClient;
using namespace NTableClient;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TString TryGetStickyProxyAddress(const ITransactionPtr& transaction)
{
    return transaction
        ->As<NRpcProxy::TTransaction>()
        ->GetStickyProxyAddress();
}

TString GetStickyProxyAddress(const ITransactionPtr& transaction)
{
    auto address = TryGetStickyProxyAddress(transaction);
    EXPECT_TRUE(address);
    return address;
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TApiTestBase, TestDuplicateTransactionId)
{
    TTransactionStartOptions options{
        .Id = MakeRandomId(EObjectType::AtomicTabletTransaction, MinValidCellTag)
    };

    auto transaction1 = WaitFor(Client_->StartTransaction(NTransactionClient::ETransactionType::Tablet, options))
        .ValueOrThrow();

    bool found = false;
    // There are several proxies in the environment and
    // the only one of them will return the error,
    // so try start several times to catch it.
    for (int i = 0; i < 32; ++i) {
        auto resultOrError = WaitFor(Client_->StartTransaction(NTransactionClient::ETransactionType::Tablet, options));
        if (resultOrError.IsOK()) {
            auto transaction2 = resultOrError
                .Value();
            EXPECT_FALSE(GetStickyProxyAddress(transaction1) == GetStickyProxyAddress(transaction2));
        } else {
            EXPECT_FALSE(NRpcProxy::IsRetriableError(resultOrError));
            found = true;
        }
    }
    EXPECT_TRUE(found);

    WaitFor(transaction1->Commit())
        .ValueOrThrow();
}

TEST_F(TApiTestBase, TestStartTimestamp)
{
    auto timestamp = WaitFor(Client_->GetTimestampProvider()->GenerateTimestamps())
        .ValueOrThrow();

    TTransactionStartOptions options{
        .StartTimestamp = timestamp
    };

    auto transaction = WaitFor(Client_->StartTransaction(NTransactionClient::ETransactionType::Tablet, options))
        .ValueOrThrow();

    EXPECT_EQ(timestamp, transaction->GetStartTimestamp());
}

TEST_F(TApiTestBase, TestTransactionProxyAddress)
{
    // Prepare for tests: discover some proxy address.
    auto proxyAddress = GetStickyProxyAddress(WaitFor(Client_->StartTransaction(
        NTransactionClient::ETransactionType::Tablet))
        .ValueOrThrow());
    // Tablet transaction supports sticky proxy address.
    {
        auto transaction = WaitFor(Client_->StartTransaction(
            NTransactionClient::ETransactionType::Tablet))
            .ValueOrThrow();
        EXPECT_TRUE(TryGetStickyProxyAddress(transaction));
    }
    // Master transaction does not support sticky proxy address.
    {
        auto transaction = WaitFor(Client_->StartTransaction(
            NTransactionClient::ETransactionType::Master))
            .ValueOrThrow();
        EXPECT_FALSE(TryGetStickyProxyAddress(transaction));
    }
    // Attachment to master transaction with specified sticky proxy address is not supported.
    {
        auto transaction = WaitFor(Client_->StartTransaction(
            NTransactionClient::ETransactionType::Master))
            .ValueOrThrow();

        TTransactionAttachOptions attachOptions{.StickyAddress = proxyAddress};
        EXPECT_THROW(Client_->AttachTransaction(transaction->GetId(), attachOptions), TErrorException);

        // Sanity check.
        Client_->AttachTransaction(transaction->GetId());
    }
    // Attached tablet transaction must be recognized as sticky (in particular, must support sticky proxy address)
    // even if sticky address option has been not provided during attachment explicitly.
    {
        auto transaction = WaitFor(Client_->StartTransaction(
            NTransactionClient::ETransactionType::Tablet))
            .ValueOrThrow();
        bool found = false;
        // Try attach several times to choose proper proxy implicitly.
        for (int i = 0; i < 32; ++i) {
            ITransactionPtr transaction2;
            try {
                transaction2 = Client_->AttachTransaction(transaction->GetId());
            } catch (const std::exception&) {
                continue;
            }
            EXPECT_EQ(GetStickyProxyAddress(transaction), GetStickyProxyAddress(transaction2));
            found = true;
        }
        EXPECT_TRUE(found);
    }
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TModifyRowsTest, TestAttachTabletTransaction)
{
    auto transaction = WaitFor(Client_->StartTransaction(
        NTransactionClient::ETransactionType::Tablet))
        .ValueOrThrow();

    auto proxyAddress = GetStickyProxyAddress(transaction);

    // Sanity check that the environment contains at least two proxies
    // and that the transaction start changes target proxy over time.
    {
        bool foundSecondProxy = false;
        for (int i = 0; i < 32; ++i) {
            auto transaction2 = WaitFor(Client_->StartTransaction(
                NTransactionClient::ETransactionType::Tablet))
                .ValueOrThrow();
            if (GetStickyProxyAddress(transaction2) != proxyAddress) {
                foundSecondProxy = true;
                break;
            }
        }
        EXPECT_TRUE(foundSecondProxy);
    }

    TTransactionAttachOptions attachOptions{.StickyAddress = proxyAddress};

    // Transaction attachment.
    auto transaction2 = Client_->AttachTransaction(
        transaction->GetId(),
        attachOptions);
    EXPECT_EQ(proxyAddress, GetStickyProxyAddress(transaction2));
    EXPECT_EQ(transaction->GetId(), transaction2->GetId());

    auto transaction3 = Client_->AttachTransaction(
        transaction->GetId(),
        attachOptions);
    EXPECT_EQ(proxyAddress, GetStickyProxyAddress(transaction3));
    EXPECT_EQ(transaction->GetId(), transaction3->GetId());

    // Independent writes from several sources.
    std::vector<std::pair<i64, i64>> expectedContent;

    for (int i = 0; i < 10; ++i) {
        WriteSimpleRow(transaction, 0 + i, 10 + i, /*sequenceNumber*/ std::nullopt);
        expectedContent.emplace_back(0 + i, 10 + i);
        WriteSimpleRow(transaction2, 100 + i, 110 + i, /*sequenceNumber*/ std::nullopt);
        expectedContent.emplace_back(100 + i, 110 + i);
    }

    // #FlushModifications as opposed to #Flush does not change the transaction state within RPC proxy
    // allowing to send modifications from the second transaction afterward.
    WaitFor(transaction->As<NRpcProxy::TTransaction>()->FlushModifications())
        .ThrowOnError();

    for (int i = 0; i < 10; ++i) {
        expectedContent.emplace_back(200 + i, 220 + i);
        WriteSimpleRow(transaction2, 200 + i, 220 + i, /*sequenceNumber*/ std::nullopt);
    }

    // Double-flush.
    EXPECT_THROW(WaitFor(transaction->As<NRpcProxy::TTransaction>()->FlushModifications()).ThrowOnError(), TErrorException);

    ValidateTableContent({});

    WaitFor(transaction2->Commit())
        .ValueOrThrow();

    ValidateTableContent(expectedContent);

    // Double-commit.
    WriteSimpleRow(transaction3, 4, 14, /*sequenceNumber*/ std::nullopt);
    EXPECT_THROW(WaitFor(transaction3->Commit()).ValueOrThrow(), TErrorException);
}

TEST_F(TModifyRowsTest, TestModificationsFlushedSignal)
{
    auto transaction = WaitFor(Client_->StartTransaction(
        NTransactionClient::ETransactionType::Tablet))
        .ValueOrThrow()
        ->As<NRpcProxy::TTransaction>();

    std::atomic<bool> flushed = false;
    transaction->SubscribeModificationsFlushed(BIND([&] {
        flushed = true;
    }));

    WaitFor(transaction->FlushModifications())
        .ThrowOnError();

    EXPECT_TRUE(flushed.load());
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TModifyRowsTest, TestReordering)
{
    const int rowCount = 20;

    for (int i = 0; i < rowCount; ++i) {
        WriteSimpleRow(i, i + 10);
        WriteSimpleRow(i, i + 11);
    }
    SyncCommit();

    std::vector<std::pair<i64, i64>> expected;
    for (int i = 0; i < rowCount; ++i) {
        expected.emplace_back(i, i + 11);
    }
    ValidateTableContent(expected);
}

TEST_F(TModifyRowsTest, TestIgnoringSeqNumbers)
{
    WriteSimpleRow(0, 10, 4);
    WriteSimpleRow(1, 11, 3);
    WriteSimpleRow(0, 12, 2);
    WriteSimpleRow(1, 13, -1);
    WriteSimpleRow(0, 14);
    WriteSimpleRow(1, 15, 100500);
    SyncCommit();

    ValidateTableContent({{0, 14}, {1, 15}});
}

////////////////////////////////////////////////////////////////////////////////

class TMultiLookupTest
    : public TDynamicTablesTestBase
{
public:
    static void SetUpTestCase()
    {
        auto configPath = TString(std::getenv("YT_DRIVER_CONFIG_PATH"));
        YT_VERIFY(configPath);
        IMapNodePtr config;
        {
            TIFStream configInStream(configPath);
            config = ConvertToNode(&configInStream)->AsMap();
        }
        config->AddChild("enable_multi_lookup", ConvertToNode(true));
        {
            TOFStream configOutStream(configPath);
            configOutStream << ConvertToYsonString(config).ToString() << Endl;
        }

        TDynamicTablesTestBase::SetUpTestCase();

        CreateTable(
            "//tmp/multi_lookup_test", // tablePath
            "[" // schema
            "{name=k0;type=int64;sort_order=ascending};"
            "{name=v1;type=int64};]"
        );
    }

    static void TearDownTestCase()
    {
        TDynamicTablesTestBase::TearDownTestCase();
    }
};

TEST_F(TMultiLookupTest, TestMultiLookup)
{
    WriteUnversionedRow(
        {"k0", "v1"},
        "<id=0> 0; <id=1> 0;");
    WriteUnversionedRow(
        {"k0", "v1"},
        "<id=0> 1; <id=1> 1");

    auto key0 = PrepareUnversionedRow(
        {"k0", "v1"},
        "<id=0> 0;");
    auto key1 = PrepareUnversionedRow(
        {"k0", "v1"},
        "<id=0; ts=2> 1;");

    std::vector<TMultiLookupSubrequest> subrequests;
    subrequests.push_back({
        Table_,
        std::get<1>(key0),
        std::get<0>(key0),
        TLookupRowsOptions()});
    subrequests.push_back({
        Table_,
        std::get<1>(key1),
        std::get<0>(key1),
        TLookupRowsOptions()});

    auto rowsets = WaitFor(Client_->MultiLookup(
        subrequests,
        TMultiLookupOptions()))
        .ValueOrThrow();

    ASSERT_EQ(2u, rowsets.size());

    ASSERT_EQ(1u, rowsets[0]->GetRows().Size());
    ASSERT_EQ(1u, rowsets[1]->GetRows().Size());

    auto expected = ToString(YsonToSchemalessRow("<id=0> 0; <id=1> 0;"));
    auto actual = ToString(rowsets[0]->GetRows()[0]);
    EXPECT_EQ(expected, actual);

    expected = ToString(YsonToSchemalessRow("<id=0> 1; <id=1> 1;"));
    actual = ToString(rowsets[1]->GetRows()[0]);
    EXPECT_EQ(expected, actual);
}

////////////////////////////////////////////////////////////////////////////////

static constexpr auto SERVICE_TICKET =
    "3:serv:CBAQ__________9_IgYIlJEGECo:O9-vbod_8czkKrpwJAZCI8UgOIhNr2xKPcS-LWALrVC224jga2nIT6vLiw6q3d6pAT60g9K7NB39LEmh7vMuePtUMjzuZuL-uJg17BsH2iTLCZSxDjWxbU9piA2T6u607jiSyiy-FI74pEPqkz7KKJ28aPsefuC1VUweGkYFzNY";

NAuth::TTvmClientPtr CreateTvmClient() {
    NTvmAuth::TMockedUpdater::TSettings settings;
    settings.SelfTvmId = 100500;
    settings.Backends = {
        {
            /*.Alias_ = */ "my_dest",
            /*.Id_ = */ 2031010,
            /*.Value_ = */ SERVICE_TICKET,
        },
    };
    
    return std::make_shared<NTvmAuth::TTvmClient>(new NTvmAuth::TMockedUpdater(settings));
}

class TServiceTicketAuthTestWrapper
    : public NAuth::TServiceTicketClientAuth
{
public:
    TServiceTicketAuthTestWrapper(const NAuth::TTvmClientPtr& tvmClient)
        : NAuth::TServiceTicketClientAuth(tvmClient)
    { }

    virtual TString IssueServiceTicket() override
    {
        auto ticket = NAuth::TServiceTicketClientAuth::IssueServiceTicket();
        IssuedServiceTickets_.push_back(ticket);
        return ticket;
    }

    const std::vector<TString>& GetIssuedServiceTickets() const
    {
        return IssuedServiceTickets_;
    }

private:
    std::vector<TString> IssuedServiceTickets_;
};

TEST_F(TApiTestBase, TestTvmServiceTicketAuth)
{
    auto serviceTicketAuth = New<TServiceTicketAuthTestWrapper>(CreateTvmClient());
    auto clientOptions = TClientOptions::FromServiceTicketAuth(serviceTicketAuth);

    auto client = Connection_->CreateClient(clientOptions);

    client->CreateNode("//tmp/test_node", NObjectClient::EObjectType::MapNode).Get();

    auto issuedTickets = serviceTicketAuth->GetIssuedServiceTickets();
    EXPECT_GT(issuedTickets.size(), 0U);
    EXPECT_EQ(issuedTickets.front(), SERVICE_TICKET);
}

////////////////////////////////////////////////////////////////////////////////

class TClearTmpTestBase
    : public TApiTestBase
{
public:
    static void TearDownTestCase()
    {
        WaitFor(Client_->RemoveNode(TYPath("//tmp/*")))
            .ThrowOnError();

        TApiTestBase::TearDownTestCase();
    }
};

TEST_F(TClearTmpTestBase, TestAnyYsonValidation)
{
    TRichYPath tablePath("//tmp/test_any_validation");

    TCreateNodeOptions options;
    options.Attributes = NYTree::CreateEphemeralAttributes();
    options.Attributes->Set("schema", New<TTableSchema>(std::vector<TColumnSchema>{{"a", EValueType::Any}}));
    options.Force = true;

    // Empty yson.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedAnyValue("");
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        EXPECT_THROW_WITH_ERROR_CODE(
            WaitFor(writer->Close()).ThrowOnError(),
            NYT::NTableClient::EErrorCode::SchemaViolation);

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 0);
    }


    // Non-empty invalid yson.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedAnyValue("{foo");
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        EXPECT_THROW_WITH_ERROR_CODE(
            WaitFor(writer->Close()).ThrowOnError(),
            NYT::NTableClient::EErrorCode::SchemaViolation);

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 0);
    }

    // Composite value with invalid yson.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedCompositeValue("{foo");
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        EXPECT_THROW_WITH_ERROR_CODE(
            WaitFor(writer->Close()).ThrowOnError(),
            NYT::NTableClient::EErrorCode::SchemaViolation);

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 0);
    }

    // Valid value of another type should not be checked.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedInt64Value(42);
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }
}

TEST_F(TClearTmpTestBase, TestAnyCompatibleTypes)
{
    TRichYPath tablePath("//tmp/test_any_compatible_types");
    TCreateNodeOptions options;
    options.Attributes = NYTree::CreateEphemeralAttributes();
    options.Attributes->Set("schema", New<TTableSchema>(std::vector<TColumnSchema>{{"a", EValueType::Any}}));
    options.Force = true;

    // Null.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedNullValue();
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // Int64.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedInt64Value(1);
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // Uint64.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedUint64Value(1);
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // Boolean.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedBooleanValue(false);
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // Double.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedDoubleValue(4.2);
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // String.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedStringValue("hello world!");
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // Any.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        auto ysonString = ConvertToYsonString(42);
        TUnversionedValue value = MakeUnversionedAnyValue(ysonString.AsStringBuf());
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // Composite.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedCompositeValue("[1; {a=1; b=2}]");
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        WaitFor(writer->Close())
            .ThrowOnError();

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 1);
    }

    // Min is not compatible.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedSentinelValue(EValueType::Min);
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        EXPECT_THROW_WITH_ERROR_CODE(
            WaitFor(writer->Close()).ThrowOnError(),
            NYT::NTableClient::EErrorCode::SchemaViolation);

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 0);
    }

    // Max is not compatible.

    {
        WaitFor(Client_->CreateNode(tablePath.GetPath(), EObjectType::Table, options))
            .ThrowOnError();

        auto writer = WaitFor(Client_->CreateTableWriter(tablePath))
            .ValueOrThrow();

        YT_VERIFY(writer->GetNameTable()->GetIdOrRegisterName("a") == 0);

        TUnversionedValue value = MakeUnversionedSentinelValue(EValueType::Max);
        TUnversionedOwningRow owningRow(&value, &value + 1);
        std::vector<TUnversionedRow> rows;
        rows.push_back(owningRow);
        YT_VERIFY(writer->Write(rows));
        EXPECT_THROW_WITH_ERROR_CODE(
            WaitFor(writer->Close()).ThrowOnError(),
            NYT::NTableClient::EErrorCode::SchemaViolation);

        auto rowCount = ConvertTo<i64>(WaitFor(Client_->GetNode(tablePath.GetPath() + "/@row_count"))
                                           .ValueOrThrow());
        EXPECT_EQ(rowCount, 0);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NCppTests
} // namespace NYT
