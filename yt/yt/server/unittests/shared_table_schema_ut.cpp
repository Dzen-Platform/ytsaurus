#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/server/master/table_server/shared_table_schema.h>

#include <yt/yt/client/table_client/schema.h>


namespace NYT::NTableServer {
namespace {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TEST(SharedTableSchema, Simple)
{
    auto registry = New<NTableServer::TSharedTableSchemaRegistry>();

    ASSERT_EQ(registry->GetSize(), 0);
    ASSERT_EQ(registry->GetRefCount(), 1);

    TTableSchema tableSchema1;

    auto schema11 = registry->GetSchema(TTableSchema(tableSchema1));
    auto schema12 = registry->GetSchema(TTableSchema(tableSchema1));

    ASSERT_EQ(schema11.Get(), nullptr);
    ASSERT_EQ(schema12.Get(), nullptr);

    TTableSchema tableSchema2(
        {
            TColumnSchema("foo", EValueType::String),
            TColumnSchema("bar", EValueType::Uint64),
        },
        true
    );
    auto schema21 = registry->GetSchema(TTableSchema(tableSchema2));
    auto schema22 = registry->GetSchema(TTableSchema(tableSchema2));

    ASSERT_EQ(schema21.Get(), schema22.Get());
    ASSERT_EQ(schema21->GetTableSchema(), tableSchema2);
    ASSERT_EQ(schema21->GetRefCount(), 2);
    ASSERT_EQ(registry->GetSize(), 1);
    ASSERT_EQ(registry->GetRefCount(), 2);

    TTableSchema tableSchema3(
        {
            TColumnSchema("foo", EValueType::String),
            TColumnSchema("bar", EValueType::Uint64),
        },
        false
    );
    auto schema3 = registry->GetSchema(TTableSchema(tableSchema3));
    ASSERT_EQ(schema3->GetTableSchema(), tableSchema3);
    ASSERT_NE(schema3.Get(), schema21.Get());
    ASSERT_EQ(schema3->GetRefCount(), 1);

    ASSERT_EQ(registry->GetSize(), 2);

    schema11.Reset();
    schema12.Reset();
    ASSERT_EQ(registry->GetSize(), 2);

    schema22.Reset();
    schema21.Reset();
    ASSERT_EQ(registry->GetSize(), 1);

    schema3.Reset();
    ASSERT_EQ(registry->GetSize(), 0);
    ASSERT_EQ(registry->GetRefCount(), 1);
}

////////////////////////////////////////////////////////////////////////////////

}
} // namespace NYT::NTableServer
