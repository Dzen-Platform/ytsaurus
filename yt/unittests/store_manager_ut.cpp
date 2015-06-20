#include "stdafx.h"
#include "memory_store_ut.h"

#include <ytlib/tablet_client/wire_protocol.h>
#include <ytlib/tablet_client/wire_protocol.pb.h>

#include <server/tablet_node/store_manager.h>
#include <server/tablet_node/lookup.h>

namespace NYT {
namespace NTabletNode {
namespace {

using namespace NTabletClient;
using namespace NTabletClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TStoreManagerTestBase
    : public TMemoryStoreTestBase
{
protected:
    virtual void SetUp() override
    {
        TMemoryStoreTestBase::SetUp();

        auto config = New<TTabletManagerConfig>();
        StoreManager_ = New<TStoreManager>(
            config,
            Tablet_.get(),
            BIND([=] () {
                return New<TDynamicMemoryStore>(config, TStoreId::Create(), Tablet_.get());
            }));
        Tablet_->SetStoreManager(StoreManager_);

        StoreManager_->StartEpoch(nullptr);
        StoreManager_->CreateActiveStore();
    }

    void Rotate()
    {
        StoreManager_->ScheduleRotation();
        StoreManager_->Rotate(true);
    }

    TDynamicRowRef WriteRow(
        TTransaction* transaction,
        TUnversionedRow row,
        bool prelock)
    {
        return StoreManager_->WriteRow(transaction, row, prelock);
    }

    void WriteRow(const TUnversionedOwningRow& row)
    {
        auto transaction = StartTransaction();

        StoreManager_->WriteRow(transaction.get(), row.Get(), false);

        EXPECT_EQ(1, transaction->LockedRows().size());
        auto rowRef = transaction->LockedRows()[0];

        PrepareTransaction(transaction.get());
        StoreManager_->PrepareRow(transaction.get(), rowRef);

        CommitTransaction(transaction.get());
        StoreManager_->CommitRow(transaction.get(), rowRef);
    }

    TDynamicRowRef DeleteRow(
        TTransaction* transaction,
        TUnversionedRow row,
        bool prelock)
    {
        return StoreManager_->DeleteRow(transaction, row, prelock);
    }

    void DeleteRow(const TOwningKey& key)
    {
        auto transaction = StartTransaction();

        DeleteRow(transaction.get(), key.Get(), false);

        EXPECT_EQ(1, transaction->LockedRows().size());
        auto rowRef = transaction->LockedRows()[0];

        PrepareTransaction(transaction.get());
        StoreManager_->PrepareRow(transaction.get(), rowRef);

        CommitTransaction(transaction.get());
        StoreManager_->CommitRow(transaction.get(), rowRef);
    }

    void PrepareRow(TTransaction* transaction, const TDynamicRowRef& rowRef)
    {
        StoreManager_->PrepareRow(transaction, rowRef);
    }

    void CommitRow(TTransaction* transaction, const TDynamicRowRef& rowRef)
    {
        StoreManager_->CommitRow(transaction, rowRef);
    }

    void AbortRow(TTransaction* transaction, const TDynamicRowRef& rowRef)
    {
        StoreManager_->AbortRow(transaction, rowRef);
    }

    void ConfirmRow(TTransaction* transaction, const TDynamicRowRef& rowRef)
    {
        StoreManager_->ConfirmRow(transaction, rowRef);
    }

    using TMemoryStoreTestBase::LookupRow;
    
    TUnversionedOwningRow LookupRow(const TOwningKey& key, TTimestamp timestamp)
    {
        TSharedRef request;
        {
            TReqLookupRows req;
            std::vector<TUnversionedRow> keys(1, key.Get());

            TWireProtocolWriter writer;
            writer.WriteMessage(req);
            writer.WriteUnversionedRowset(keys);

            request = MergeRefs(writer.Flush());
        }

        TSharedRef response;
        {
            TWireProtocolReader reader(request);
            TWireProtocolWriter writer;
            LookupRows(
                GetSyncInvoker(),
                Tablet_->RebuildSnapshot(),
                timestamp,
                &reader,
                &writer);
            response = MergeRefs(writer.Flush());
        }

        {
            TWireProtocolReader reader(response);
            auto row = reader.ReadUnversionedRow();
            return TUnversionedOwningRow(row);
        }
    }


    TStoreManagerPtr StoreManager_;

};

///////////////////////////////////////////////////////////////////////////////

class TSingleLockStoreManagerTest
    : public TStoreManagerTestBase
{ };

TEST_F(TSingleLockStoreManagerTest, EmptyWriteFailure)
{
    EXPECT_ANY_THROW({
        WriteRow(BuildKey("key=1"));
    });
}

TEST_F(TSingleLockStoreManagerTest, PrelockRow)
{
    auto store = Tablet_->GetActiveStore();
    EXPECT_EQ(0, store->GetLockCount());

    auto transaction = StartTransaction();

    auto rowRef = WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), true);

    EXPECT_EQ(1, store->GetLockCount());
    EXPECT_EQ(0, transaction->LockedRows().size());
    EXPECT_EQ(store, rowRef.Store);

    ConfirmRow(transaction.get(), rowRef);
    EXPECT_EQ(1, transaction->LockedRows().size());
    EXPECT_EQ(rowRef, transaction->LockedRows()[0]);
    EXPECT_EQ(1, store->GetLockCount());
}

TEST_F(TSingleLockStoreManagerTest, AbortRow)
{
    auto store = Tablet_->GetActiveStore();
    EXPECT_EQ(0, store->GetLockCount());

    auto transaction = StartTransaction();

    WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), false);

    EXPECT_EQ(1, transaction->LockedRows().size());
    EXPECT_EQ(1, store->GetLockCount());

    auto rowRef = transaction->LockedRows()[0];

    AbortTransaction(transaction.get());
    AbortRow(transaction.get(), rowRef);

    EXPECT_EQ(0, store->GetLockCount());
}

TEST_F(TSingleLockStoreManagerTest, CommitRow)
{
    auto store = Tablet_->GetActiveStore();
    EXPECT_EQ(0, store->GetLockCount());

    auto transaction = StartTransaction();

    auto rowRef = WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), false);

    EXPECT_EQ(1, store->GetLockCount());
    EXPECT_EQ(store, rowRef.Store);

    PrepareTransaction(transaction.get());
    PrepareRow(transaction.get(), rowRef);

    CommitTransaction(transaction.get());
    CommitRow(transaction.get(), rowRef);

    EXPECT_EQ(0, store->GetLockCount());
}

TEST_F(TSingleLockStoreManagerTest, ConfirmRowWithRotation)
{
    auto store1 = Tablet_->GetActiveStore();

    auto transaction = StartTransaction();

    auto rowRef1 = WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), true);
    EXPECT_EQ(0, transaction->LockedRows().size());
    EXPECT_EQ(store1, rowRef1.Store);

    Rotate();
    auto store2 = Tablet_->GetActiveStore();

    EXPECT_NE(store1, store2);
    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    ConfirmRow(transaction.get(), rowRef1);

    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto rowRef2 = transaction->LockedRows()[0];
    EXPECT_EQ(store1, rowRef2.Store);

    PrepareTransaction(transaction.get());
    PrepareRow(transaction.get(), rowRef2);

    CommitTransaction(transaction.get());
    CommitRow(transaction.get(), rowRef2);

    EXPECT_EQ(0, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto key = BuildKey("1");
    auto row = Stroka("key=1;a=1");
    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), row));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), row));
}

TEST_F(TSingleLockStoreManagerTest, PrepareRowWithRotation)
{
    auto store1 = Tablet_->GetActiveStore();

    auto transaction = StartTransaction();

    WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), false);
    EXPECT_EQ(1, transaction->LockedRows().size());

    Rotate();
    auto store2 = Tablet_->GetActiveStore();

    EXPECT_NE(store1, store2);
    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto rowRef = transaction->LockedRows()[0];
    EXPECT_EQ(store1, rowRef.Store);

    PrepareTransaction(transaction.get());
    PrepareRow(transaction.get(), rowRef);
    EXPECT_EQ(store1, rowRef.Store);

    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    CommitTransaction(transaction.get());
    CommitRow(transaction.get(), rowRef);

    EXPECT_EQ(0, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto key = BuildKey("1");
    auto row = Stroka("key=1;a=1");
    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), row));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), row));
}

TEST_F(TSingleLockStoreManagerTest, MigrateRow)
{
    auto store1 = Tablet_->GetActiveStore();

    auto transaction = StartTransaction();

    WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), false);
    EXPECT_EQ(1, transaction->LockedRows().size());

    auto& rowRef = transaction->LockedRows()[0];
    EXPECT_EQ(store1, rowRef.Store);

    PrepareTransaction(transaction.get());
    PrepareRow(transaction.get(), rowRef);

    Rotate();
    auto store2 = Tablet_->GetActiveStore();

    EXPECT_NE(store1, store2);
    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    CommitTransaction(transaction.get());
    CommitRow(transaction.get(), rowRef);

    EXPECT_EQ(0, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto key = BuildKey("1");
    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1")));
}

TEST_F(TSingleLockStoreManagerTest, WriteSameRowWithRotation)
{
    auto store1 = Tablet_->GetActiveStore();

    auto transaction = StartTransaction();

    WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), true);

    Rotate();

    EXPECT_ANY_THROW({
        WriteRow(transaction.get(), BuildRow("key=1;a=2").Get(), true);
    });
}

TEST_F(TSingleLockStoreManagerTest, DeleteSameRowWithRotation)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();

    DeleteRow(transaction.get(), key.Get(), true);

    Rotate();

    ASSERT_ANY_THROW({
        DeleteRow(transaction.get(), key.Get(), true);
    });
}

TEST_F(TSingleLockStoreManagerTest, WriteAfterDeleteFailureWithRotation)
{
    auto transaction = StartTransaction();

    DeleteRow(transaction.get(), BuildKey("1").Get(), true);

    Rotate();

    ASSERT_ANY_THROW({
        WriteRow(transaction.get(), BuildRow("key=1;a=2").Get(), true);
    });
}

TEST_F(TSingleLockStoreManagerTest, WriteWriteConflictWithRotation1)
{
    auto transaction1 = StartTransaction();
    auto transaction2 = StartTransaction();

    WriteRow(transaction1.get(), BuildRow("key=1;a=1").Get(), true);

    Rotate();

    ASSERT_ANY_THROW({
        WriteRow(transaction2.get(), BuildRow("key=1;a=1").Get(), true);
    });
}

TEST_F(TSingleLockStoreManagerTest, WriteWriteConflictWithRotation2)
{
    auto transaction1 = StartTransaction();
    auto transaction2 = StartTransaction();

    WriteRow(transaction1.get(), BuildRow("key=1;a=1").Get(), false);
    
    EXPECT_EQ(1, transaction1->LockedRows().size());
    auto rowRef1 = transaction1->LockedRows()[0];

    PrepareTransaction(transaction1.get());
    PrepareRow(transaction1.get(), rowRef1);

    CommitTransaction(transaction1.get());
    CommitRow(transaction1.get(), rowRef1);

    Rotate();

    ASSERT_ANY_THROW({
        WriteRow(transaction2.get(), BuildRow("key=1;a=1").Get(), true);
    });
}

TEST_F(TSingleLockStoreManagerTest, WriteWriteConflictWithRotation3)
{
    auto transaction1 = StartTransaction();
    auto transaction2 = StartTransaction();

    auto store1 = Tablet_->GetActiveStore();

    WriteRow(transaction1.get(), BuildRow("key=1;a=1").Get(), true);

    Rotate();

    StoreManager_->RemoveStore(store1);

    ASSERT_ANY_THROW({
        WriteRow(transaction2.get(), BuildRow("key=1;a=1").Get(), true);
    });
}

TEST_F(TSingleLockStoreManagerTest, AbortRowWithRotation)
{
    auto store1 = Tablet_->GetActiveStore();

    auto transaction = StartTransaction();

    WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), false);
    EXPECT_EQ(1, transaction->LockedRows().size());

    Rotate();
    auto store2 = Tablet_->GetActiveStore();

    EXPECT_NE(store1, store2);
    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto& rowRef = transaction->LockedRows()[0];
    EXPECT_EQ(store1, rowRef.Store);

    AbortTransaction(transaction.get());
    AbortRow(transaction.get(), rowRef);

    EXPECT_EQ(0, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto key = BuildKey("1");
    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), Null));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), Null));
}

TEST_F(TSingleLockStoreManagerTest, LookupRow1)
{
    WriteRow(BuildRow("key=1;a=100", false));
    Rotate();
    WriteRow(BuildRow("key=1;b=3.14", false));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("1"), AsyncLastCommittedTimestamp), Stroka("key=1;a=100;b=3.14")));
}

TEST_F(TSingleLockStoreManagerTest, LookupRow2)
{
    WriteRow(BuildRow("key=1;a=100", false));
    DeleteRow(BuildKey("1"));
    Rotate();
    WriteRow(BuildRow("key=1;b=3.14", false));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("1"), AsyncLastCommittedTimestamp), Stroka("key=1;b=3.14")));
}

TEST_F(TSingleLockStoreManagerTest, LookupRow3)
{
    WriteRow(BuildRow("key=1;a=100", false));
    Rotate();
    DeleteRow(BuildKey("1"));
    WriteRow(BuildRow("key=1;b=3.14", false));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("1"), AsyncLastCommittedTimestamp), Stroka("key=1;b=3.14")));
}

TEST_F(TSingleLockStoreManagerTest, LookupRow4)
{
    WriteRow(BuildRow("key=1;a=100", false));
    Rotate();
    WriteRow(BuildRow("key=1;b=3.14", false));
    Rotate();
    WriteRow(BuildRow("key=1;a=200;c=test", false));
    Rotate();
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("1"), AsyncLastCommittedTimestamp), Stroka("key=1;a=200;b=3.14;c=test")));
}

TEST_F(TSingleLockStoreManagerTest, UnlockStoreOnCommit)
{
    auto store = Tablet_->GetActiveStore();
    auto transaction = StartTransaction();

    WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), false);
    EXPECT_EQ(1, transaction->LockedRows().size());
    auto rowRef = transaction->LockedRows()[0];

    Rotate();

    EXPECT_TRUE(StoreManager_->IsStoreLocked(store));

    PrepareTransaction(transaction.get());
    PrepareRow(transaction.get(), rowRef);
    CommitTransaction(transaction.get());
    CommitRow(transaction.get(), rowRef);

    EXPECT_FALSE(StoreManager_->IsStoreLocked(store));
}

TEST_F(TSingleLockStoreManagerTest, UnlockStoreOnAbort)
{
    auto store = Tablet_->GetActiveStore();
    auto transaction = StartTransaction();

    WriteRow(transaction.get(), BuildRow("key=1;a=1").Get(), false);
    EXPECT_EQ(1, transaction->LockedRows().size());
    auto rowRef = transaction->LockedRows()[0];

    Rotate();

    EXPECT_TRUE(StoreManager_->IsStoreLocked(store));

    AbortTransaction(transaction.get());
    AbortRow(transaction.get(), rowRef);

    EXPECT_FALSE(StoreManager_->IsStoreLocked(store));
}

TEST_F(TSingleLockStoreManagerTest, WriteRotateWrite)
{
    auto store1 = Tablet_->GetActiveStore();
    EXPECT_EQ(0, store1->GetLockCount());

    auto transaction1 = StartTransaction();
    auto transaction2 = StartTransaction();

    auto rowRef1 = WriteRow(transaction1.get(), BuildRow("key=1;a=1").Get(), false);
    EXPECT_EQ(store1, rowRef1.Store);

    auto rowRef2 = WriteRow(transaction2.get(), BuildRow("key=2;a=2").Get(), false);
    EXPECT_EQ(store1, rowRef2.Store);

    EXPECT_EQ(2, store1->GetLockCount());

    PrepareTransaction(transaction2.get());
    PrepareRow(transaction2.get(), rowRef2);
    CommitTransaction(transaction2.get());
    CommitRow(transaction2.get(), rowRef2);

    EXPECT_EQ(1, store1->GetLockCount());

    Rotate();
    auto store2 = Tablet_->GetActiveStore();
    EXPECT_NE(store1, store2);

    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto transaction3 = StartTransaction();

    auto rowRef3 = WriteRow(transaction3.get(), BuildRow("key=2;a=3").Get(), false);
    EXPECT_EQ(store2, rowRef3.Store);

    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(1, store2->GetLockCount());

    PrepareTransaction(transaction3.get());
    PrepareRow(transaction3.get(), rowRef3);
    CommitTransaction(transaction3.get());
    CommitRow(transaction3.get(), rowRef3);

    PrepareTransaction(transaction1.get());
    PrepareRow(transaction1.get(), rowRef1);
    CommitTransaction(transaction1.get());
    CommitRow(transaction1.get(), rowRef1);

    EXPECT_EQ(0, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());
}

TEST_F(TSingleLockStoreManagerTest, WriteBlockedWrite)
{
    auto store = Tablet_->GetActiveStore();
    EXPECT_EQ(0, store->GetLockCount());

    auto transaction1 = StartTransaction();
    auto rowRef1 = WriteRow(transaction1.get(), BuildRow("key=1;a=1").Get(), false);
    EXPECT_EQ(store, rowRef1.Store);

    PrepareTransaction(transaction1.get());
    PrepareRow(transaction1.get(), rowRef1);

    auto transaction2 = StartTransaction(transaction1->GetPrepareTimestamp() + 10);

    EXPECT_THROW({
        WriteRow(transaction2.get(), BuildRow("key=1;a=1").Get(), true);
    }, TRowBlockedException);

    EXPECT_EQ(1, store->GetLockCount());
}

///////////////////////////////////////////////////////////////////////////////

class TSingleLockStoreManagerTestWithStringKeys
    : public TSingleLockStoreManagerTest
{
protected:
    virtual TKeyColumns GetKeyColumns() const
    {
        TKeyColumns keyColumns;
        keyColumns.push_back("key");
        return keyColumns;
    }

    virtual TTableSchema GetSchema() const
    {
        // NB: Key columns must go first.
        TTableSchema schema;
        schema.Columns().push_back(TColumnSchema("key", EValueType::String));
        schema.Columns().push_back(TColumnSchema("a", EValueType::Int64));
        schema.Columns().push_back(TColumnSchema("b", EValueType::Double));
        schema.Columns().push_back(TColumnSchema("c", EValueType::String));
        return schema;
    }

};

TEST_F(TSingleLockStoreManagerTestWithStringKeys, StringKey)
{
    WriteRow(BuildRow("key=test;a=100", false));
    WriteRow(BuildRow("key=another_test;a=101", false));
    WriteRow(BuildRow("b=3.14", false));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("test"), AsyncLastCommittedTimestamp), Stroka("key=test;a=100")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("another_test"), AsyncLastCommittedTimestamp), Stroka("key=another_test;a=101")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("weird_test"), AsyncLastCommittedTimestamp), Null));
}

TEST_F(TSingleLockStoreManagerTestWithStringKeys, NullKey)
{
    WriteRow(BuildRow("key=test;a=100", false));
    WriteRow(BuildRow("key=another_test;a=101", false));
    WriteRow(BuildRow("b=3.14", false));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("<type=null>#"), AsyncLastCommittedTimestamp), Stroka("b=3.14")));
}

///////////////////////////////////////////////////////////////////////////////

class TSingleLockStoreManagerTestWithCompositeKeys
    : public TSingleLockStoreManagerTest
{
protected:
    virtual TKeyColumns GetKeyColumns() const
    {
        TKeyColumns keyColumns;
        keyColumns.push_back("k1");
        keyColumns.push_back("k2");
        return keyColumns;
    }

    virtual TTableSchema GetSchema() const
    {
        // NB: Key columns must go first.
        TTableSchema schema;
        schema.Columns().push_back(TColumnSchema("k1", EValueType::Int64));
        schema.Columns().push_back(TColumnSchema("k2", EValueType::Int64));
        schema.Columns().push_back(TColumnSchema("v", EValueType::Int64));
        return schema;
    }

};

TEST_F(TSingleLockStoreManagerTestWithCompositeKeys, Write)
{
    WriteRow(BuildRow("k1=1;k2=1;v=100", false));
    WriteRow(BuildRow("k1=1;k2=2;v=200", false));
    WriteRow(BuildRow("k1=2;k2=1;v=300", false));
    WriteRow(BuildRow("k1=2;k2=2;v=400", false));

    WriteRow(BuildRow("     k2=2;v=500", false));
    WriteRow(BuildRow("k1=2;     v=600", false));
    WriteRow(BuildRow("          v=700", false));

    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("1;1"), AsyncLastCommittedTimestamp), Stroka("k1=1;k2=1;v=100")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("1;2"), AsyncLastCommittedTimestamp), Stroka("k1=1;k2=2;v=200")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("2;1"), AsyncLastCommittedTimestamp), Stroka("k1=2;k2=1;v=300")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("2;2"), AsyncLastCommittedTimestamp), Stroka("k1=2;k2=2;v=400")));

    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("<type=null>#;           2"), AsyncLastCommittedTimestamp), Stroka("     k2=2;v=500")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("2;           <type=null>#"), AsyncLastCommittedTimestamp), Stroka("k1=2;     v=600")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(BuildKey("<type=null>#;<type=null>#"), AsyncLastCommittedTimestamp), Stroka("          v=700")));
}

///////////////////////////////////////////////////////////////////////////////

class TMultiLockStoreManagerTest
    : public TStoreManagerTestBase
{
protected:
    virtual TTableSchema GetSchema() const
    {
        // NB: Key columns must go first.
        TTableSchema schema;
        schema.Columns().push_back(TColumnSchema("key", EValueType::Int64));
        schema.Columns().push_back(TColumnSchema("a", EValueType::Int64, Stroka("l1")));
        schema.Columns().push_back(TColumnSchema("b", EValueType::Double, Stroka("l2")));
        schema.Columns().push_back(TColumnSchema("c", EValueType::String));
        return schema;
    }

};

TEST_F(TMultiLockStoreManagerTest, WriteTakesPrimaryLock)
{
    auto store = Tablet_->GetActiveStore();
    auto transaction = StartTransaction();
    auto* transaction_ = transaction.get();
    auto row = WriteRow(transaction_, BuildRow("key=1;c=text", false).Get(), false).Row;
    EXPECT_EQ(transaction_, GetLock(row, 0).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 1).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 2).Transaction);
    EXPECT_EQ(1, store->GetLockCount());
}

TEST_F(TMultiLockStoreManagerTest, WriteTakesSecondaryLocks1)
{
    auto store = Tablet_->GetActiveStore();
    auto transaction = StartTransaction();
    auto* transaction_ = transaction.get();
    auto row = WriteRow(transaction_, BuildRow("key=1;a=1", false).Get(), false).Row;
    EXPECT_EQ(nullptr, GetLock(row, 0).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 1).Transaction);
    EXPECT_EQ(nullptr, GetLock(row, 2).Transaction);
    EXPECT_EQ(1, store->GetLockCount());
}

TEST_F(TMultiLockStoreManagerTest, WriteTakesSecondaryLocks2)
{
    auto store = Tablet_->GetActiveStore();
    auto transaction = StartTransaction();
    auto* transaction_ = transaction.get();
    auto row = WriteRow(transaction_, BuildRow("key=1;b=3.14", false).Get(), false).Row;
    EXPECT_EQ(nullptr, GetLock(row, 0).Transaction);
    EXPECT_EQ(nullptr, GetLock(row, 1).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 2).Transaction);
    EXPECT_EQ(1, store->GetLockCount());
}

TEST_F(TMultiLockStoreManagerTest, WriteTakesSecondaryLocks3)
{
    auto store = Tablet_->GetActiveStore();
    auto transaction = StartTransaction();
    auto* transaction_ = transaction.get();
    auto row = WriteRow(transaction_, BuildRow("key=1;a=1;b=3.14", false).Get(), false).Row;
    EXPECT_EQ(nullptr, GetLock(row, 0).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 1).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 2).Transaction);
    EXPECT_EQ(1, store->GetLockCount());
}

TEST_F(TMultiLockStoreManagerTest, DeleteTakesPrimaryLock)
{
    auto store = Tablet_->GetActiveStore();
    auto transaction = StartTransaction();
    auto* transaction_ = transaction.get();
    auto row = DeleteRow(transaction_, BuildKey("1").Get(), false).Row;
    EXPECT_EQ(transaction_, GetLock(row, 0).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 1).Transaction);
    EXPECT_EQ(transaction_, GetLock(row, 2).Transaction);
    EXPECT_EQ(1, store->GetLockCount());
}

TEST_F(TMultiLockStoreManagerTest, MigrateRow1)
{
    auto key = BuildKey("1");

    auto store1 = Tablet_->GetActiveStore();

    auto transaction1 = StartTransaction();
    WriteRow(transaction1.get(), BuildRow("key=1;a=1", false).Get(), false);
    EXPECT_EQ(1, transaction1->LockedRows().size());
    auto& rowRef1 = transaction1->LockedRows()[0];
    EXPECT_EQ(store1, rowRef1.Store);

    auto transaction2 = StartTransaction();
    WriteRow(transaction2.get(), BuildRow("key=1;b=3.14", false).Get(), false);
    EXPECT_EQ(1, transaction2->LockedRows().size());
    auto& rowRef2 = transaction2->LockedRows()[0];
    EXPECT_EQ(store1, rowRef1.Store);

    EXPECT_EQ(rowRef1.Row, rowRef2.Row);

    PrepareTransaction(transaction1.get());
    PrepareRow(transaction1.get(), rowRef1);

    PrepareTransaction(transaction2.get());
    PrepareRow(transaction2.get(), rowRef2);

    Rotate();
    auto store2 = Tablet_->GetActiveStore();

    EXPECT_NE(store1, store2);
    EXPECT_EQ(2, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    CommitTransaction(transaction1.get());
    CommitRow(transaction1.get(), rowRef1);

    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1")));

    CommitTransaction(transaction2.get());
    CommitRow(transaction2.get(), rowRef2);

    EXPECT_EQ(0, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1;b=3.14")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1;b=3.14")));
}

TEST_F(TMultiLockStoreManagerTest, MigrateRow2)
{
    auto key = BuildKey("1");

    auto store1 = Tablet_->GetActiveStore();

    auto transaction1 = StartTransaction();
    WriteRow(transaction1.get(), BuildRow("key=1;a=1", false).Get(), false);
    EXPECT_EQ(1, transaction1->LockedRows().size());
    auto& rowRef1 = transaction1->LockedRows()[0];
    EXPECT_EQ(store1, rowRef1.Store);

    PrepareTransaction(transaction1.get());
    PrepareRow(transaction1.get(), rowRef1);

    Rotate();
    auto store2 = Tablet_->GetActiveStore();

    EXPECT_NE(store1, store2);
    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    auto transaction2 = StartTransaction();
    WriteRow(transaction2.get(), BuildRow("key=1;b=3.14", false).Get(), false);
    EXPECT_EQ(1, transaction2->LockedRows().size());
    auto& rowRef2 = transaction2->LockedRows()[0];
    EXPECT_EQ(store1, rowRef1.Store);

    EXPECT_NE(rowRef1.Row, rowRef2.Row);

    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(1, store2->GetLockCount());

    PrepareTransaction(transaction2.get());
    PrepareRow(transaction2.get(), rowRef2);

    CommitTransaction(transaction2.get());
    CommitRow(transaction2.get(), rowRef2);

    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), Null));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), Stroka("key=1;b=3.14")));

    EXPECT_EQ(1, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    CommitTransaction(transaction1.get());
    CommitRow(transaction1.get(), rowRef1);

    EXPECT_EQ(0, store1->GetLockCount());
    EXPECT_EQ(0, store2->GetLockCount());

    EXPECT_TRUE(AreRowsEqual(LookupRow(store1, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1")));
    EXPECT_TRUE(AreRowsEqual(LookupRow(store2, key, AsyncLastCommittedTimestamp), Stroka("key=1;a=1;b=3.14")));
}

///////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NTabletNode
} // namespace NYT

