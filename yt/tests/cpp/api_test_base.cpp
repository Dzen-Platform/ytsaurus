#include "api_test_base.h"

#include <yt/client/api/rowset.h>
#include <yt/client/api/transaction.h>

#include <yt/client/table_client/helpers.h>
#include <yt/client/table_client/name_table.h>

#include <yt/core/logging/config.h>
#include <yt/core/logging/log_manager.h>

namespace NYT {
namespace NCppTests {

using namespace NApi;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NSecurityClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

void TApiTestBase::SetUpTestCase()
{
    const auto* configPath = std::getenv("YT_CONSOLE_DRIVER_CONFIG_PATH");
    TIFStream configStream(configPath);
    auto config = ConvertToNode(&configStream)->AsMap();

    if (auto logging = config->FindChild("logging")) {
        NLogging::TLogManager::Get()->Configure(ConvertTo<NLogging::TLogConfigPtr>(logging));
    }

    Connection_ = NApi::CreateConnection(config->GetChild("driver"));

    CreateClient(NRpc::RootUserName);
}

void TApiTestBase::TearDownTestCase()
{
    Client_.Reset();
    Connection_.Reset();
}

void TApiTestBase::CreateClient(const TString& userName)
{
    TClientOptions clientOptions;
    clientOptions.PinnedUser = userName;
    Client_ = Connection_->CreateClient(clientOptions);
}

NApi::IConnectionPtr TApiTestBase::Connection_;
NApi::IClientPtr TApiTestBase::Client_;

////////////////////////////////////////////////////////////////////////////////

void TDynamicTablesTestBase::TearDownTestCase()
{
    SyncUnmountTable(Table_);

    WaitFor(Client_->RemoveNode(TYPath("//tmp/*")))
        .ThrowOnError();

    RemoveTabletCells();

    RemoveSystemObjects("//sys/tablet_cell_bundles", [] (const TString& name) {
        return name != "default";
    });

    WaitFor(Client_->SetNode(TYPath("//sys/accounts/tmp/@resource_limits/tablet_count"), ConvertToYsonString(0)))
        .ThrowOnError();

    TApiTestBase::TearDownTestCase();
}

void TDynamicTablesTestBase::SetUpTestCase()
{
    TApiTestBase::SetUpTestCase();

    auto cellId = WaitFor(Client_->CreateObject(EObjectType::TabletCell))
        .ValueOrThrow();
    WaitUntilEqual(TYPath("#") + ToString(cellId) + "/@health", "good");

    WaitFor(Client_->SetNode(TYPath("//sys/accounts/tmp/@resource_limits/tablet_count"), ConvertToYsonString(1000)))
        .ThrowOnError();
}

void TDynamicTablesTestBase::CreateTableAndClient(
    const TString& tablePath,
    const TString& schema,
    const TString& userName)
{
    // Client for root is already created in TApiTestBase::SetUpTestCase
    if (userName != RootUserName) {
        CreateClient(userName);
    }

    Table_ = tablePath;
    ASSERT_TRUE(tablePath.StartsWith("//tmp"));

    auto attributes = TYsonString("{dynamic=%true;schema=" + schema + "}");
    TCreateNodeOptions options;
    options.Attributes = ConvertToAttributes(attributes);

    WaitFor(Client_->CreateNode(Table_, EObjectType::Table, options))
        .ThrowOnError();

    SyncMountTable(Table_);
}

void TDynamicTablesTestBase::SyncMountTable(const TYPath& path)
{
    WaitFor(Client_->MountTable(path))
        .ThrowOnError();
    WaitUntilEqual(path + "/@tablet_state", "mounted");
}

void TDynamicTablesTestBase::SyncUnmountTable(const TYPath& path)
{
    WaitFor(Client_->UnmountTable(path))
        .ThrowOnError();
    WaitUntilEqual(path + "/@tablet_state", "unmounted");
}

void TDynamicTablesTestBase::WaitUntilEqual(const TYPath& path, const TString& expected)
{
    WaitUntil(
        [&] {
            auto value = WaitFor(Client_->GetNode(path))
                .ValueOrThrow();
            return ConvertTo<IStringNodePtr>(value)->GetValue() == expected;
        },
        Format("%Qv is not %Qv", path, expected));
}

void TDynamicTablesTestBase::WaitUntil(
    std::function<bool()> predicate,
    const TString& errorMessage)
{
    auto start = Now();
    bool reached = false;
    for (int attempt = 0; attempt < 2*30; ++attempt) {
        if (predicate()) {
            reached = true;
            break;
        }
        Sleep(TDuration::MilliSeconds(500));
    }

    if (!reached) {
        THROW_ERROR_EXCEPTION("%v after %v seconds",
            errorMessage,
            (Now() - start).Seconds());
    }
}

std::tuple<TSharedRange<TUnversionedRow>, TNameTablePtr> TDynamicTablesTestBase::PrepareUnversionedRow(
    const std::vector<TString>& names,
    const TString& rowString)
{
    auto nameTable = New<TNameTable>();
    for (const auto& name : names) {
        nameTable->GetIdOrRegisterName(name);
    }

    auto rowBuffer = New<TRowBuffer>();
    auto owningRow = YsonToSchemalessRow(rowString);
    std::vector<TUnversionedRow> rows{rowBuffer->Capture(owningRow.Get())};
    return std::make_tuple(MakeSharedRange(rows, std::move(rowBuffer)), std::move(nameTable));
}

void TDynamicTablesTestBase::WriteUnversionedRow(
    std::vector<TString> names,
    const TString& rowString)
{
    auto preparedRow = PrepareUnversionedRow(names, rowString);
    WriteRows(
        std::get<1>(preparedRow),
        std::get<0>(preparedRow));
}

void TDynamicTablesTestBase::WriteRows(
    TNameTablePtr nameTable,
    TSharedRange<TUnversionedRow> rows)
{
    auto transaction = WaitFor(Client_->StartTransaction(NTransactionClient::ETransactionType::Tablet))
        .ValueOrThrow();

    transaction->WriteRows(
        Table_,
        nameTable,
        rows);

    auto commitResult = WaitFor(transaction->Commit())
        .ValueOrThrow();

    const auto& timestamps = commitResult.CommitTimestamps.Timestamps;
    ASSERT_EQ(1, timestamps.size());
}

std::tuple<TSharedRange<TVersionedRow>, TNameTablePtr> TDynamicTablesTestBase::PrepareVersionedRow(
    const std::vector<TString>& names,
    const TString& keyYson,
    const TString& valueYson)
{
    auto nameTable = New<TNameTable>();
    for (const auto& name : names) {
        nameTable->GetIdOrRegisterName(name);
    }

    auto rowBuffer = New<TRowBuffer>();
    auto row = YsonToVersionedRow(rowBuffer, keyYson, valueYson);
    std::vector<TVersionedRow> rows{row};
    return std::make_tuple(MakeSharedRange(rows, std::move(rowBuffer)), std::move(nameTable));
}

void TDynamicTablesTestBase::WriteVersionedRow(
    std::vector<TString> names,
    const TString& keyYson,
    const TString& valueYson)
{
    auto preparedRow = PrepareVersionedRow(names, keyYson, valueYson);
    WriteRows(
        std::get<1>(preparedRow),
        std::get<0>(preparedRow));
}

void TDynamicTablesTestBase::WriteRows(
    TNameTablePtr nameTable,
    TSharedRange<TVersionedRow> rows)
{
    auto transaction = WaitFor(Client_->StartTransaction(NTransactionClient::ETransactionType::Tablet))
        .ValueOrThrow();

    transaction->WriteRows(
        Table_,
        nameTable,
        rows);

    auto commitResult = WaitFor(transaction->Commit())
        .ValueOrThrow();
}

void TDynamicTablesTestBase::RemoveSystemObjects(
    const TYPath& path,
    std::function<bool(const TString&)> filter)
{
    auto items = WaitFor(Client_->ListNode(path))
         .ValueOrThrow();
    auto itemsList = ConvertTo<IListNodePtr>(items);

    std::vector<TFuture<void>> asyncWait;
    for (const auto& item : itemsList->GetChildren()) {
        const auto& name = item->AsString()->GetValue();
        if (filter(name)) {
            asyncWait.push_back(Client_->RemoveNode(path + "/" + name));
        }
    }

    WaitFor(Combine(asyncWait))
        .ThrowOnError();
}

void TDynamicTablesTestBase::RemoveTabletCells(
    std::function<bool(const TString&)> filter)
{
    TYPath path = "//sys/tablet_cells";
    auto items = WaitFor(Client_->ListNode(path))
        .ValueOrThrow();
    auto itemsList = ConvertTo<IListNodePtr>(items);

    std::vector<TTabletCellId> removedCells;
    std::vector<TFuture<void>> asyncWait;
    for (const auto& item : itemsList->GetChildren()) {
        const auto& name = item->AsString()->GetValue();
        if (filter(name)) {
            removedCells.push_back(TTabletCellId::FromString(name));
            asyncWait.push_back(Client_->RemoveNode(path + "/" + name));
        }
    }

    WaitFor(Combine(asyncWait))
        .ThrowOnError();

    WaitUntil(
        [&] {
            auto value = WaitFor(Client_->ListNode(path))
                .ValueOrThrow();
            auto cells = ConvertTo<THashSet<TTabletCellId>>(value);
            for (const auto& cell : removedCells) {
                if (cells.find(cell) != cells.end()) {
                    return false;
                }
            }
            return true;
        },
        "Tablet cells are not removed");
}

TYPath TDynamicTablesTestBase::Table_;

////////////////////////////////////////////////////////////////////////////////

} // namespace NCppTests
} // namespace NYT
