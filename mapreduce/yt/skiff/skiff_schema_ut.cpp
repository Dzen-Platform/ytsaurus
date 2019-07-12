#include "checked_parser.h"
#include "checked_writer.h"
#include "skiff_schema.h"
#include "unchecked_parser.h"
#include "unchecked_writer.h"

#include <library/unittest/registar.h>

#include <util/stream/buffer.h>

using namespace NSkiff;

////////////////////////////////////////////////////////////////////////////////

template<>
void Out<TSkiffSchema>(IOutputStream& s, const TSkiffSchema& schema)
{
    s << "TSkiffSchemaPtr:" << GetShortDebugString(schema);
}

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TSkiffSchemaTestSuite) {
    Y_UNIT_TEST(TestIntEqual)
    {
        TSkiffSchemaPtr schema1 = CreateSimpleTypeSchema(EWireType::Uint64);
        schema1->SetName("schema");

        TSkiffSchemaPtr schema2 = CreateSimpleTypeSchema(EWireType::Uint64);
        schema2->SetName("schema");

        UNIT_ASSERT_VALUES_EQUAL(*schema1, *schema2);
    }

    Y_UNIT_TEST(TestTupleEqual)
    {
        TSkiffSchemaPtr schema1 = CreateTupleSchema({
            CreateSimpleTypeSchema(EWireType::Int64),
            CreateSimpleTypeSchema(EWireType::String32),
        });

        TSkiffSchemaPtr schema2 = CreateTupleSchema({
            CreateSimpleTypeSchema(EWireType::Int64),
            CreateSimpleTypeSchema(EWireType::String32),
        });

        schema1->SetName("schema");
        UNIT_ASSERT_VALUES_UNEQUAL(*schema1, *schema2);

        schema2->SetName("schema");
        UNIT_ASSERT_VALUES_EQUAL(*schema1, *schema2);
    }

    Y_UNIT_TEST(TestHashes)
    {
        TSet<size_t> hashes;

        auto schema = CreateSimpleTypeSchema(EWireType::Uint64);
        schema->SetName("schema");
        hashes.insert(THash<NSkiff::TSkiffSchema>()(*schema));

        schema = CreateSimpleTypeSchema(EWireType::Uint64);
        hashes.insert(THash<NSkiff::TSkiffSchema>()(*schema));

        auto schema2 = CreateTupleSchema({
            CreateSimpleTypeSchema(EWireType::Int64),
            CreateSimpleTypeSchema(EWireType::String32),
        });
        schema2->SetName("s");
        hashes.insert(THash<NSkiff::TSkiffSchema>()(*schema2));

        schema2->SetName("s0");
        hashes.insert(THash<NSkiff::TSkiffSchema>()(*schema2));

        schema2->SetName("s");
        hashes.insert(THash<NSkiff::TSkiffSchema>()(*schema2));

        auto schema3 = CreateRepeatedVariant16Schema({
            CreateSimpleTypeSchema(EWireType::Int64),
            schema2,
        });
        hashes.insert(THash<NSkiff::TSkiffSchema>()(*schema3));

        schema3->SetName("kek");
        hashes.insert(THash<NSkiff::TSkiffSchema>()(*schema3));

        UNIT_ASSERT_VALUES_EQUAL(hashes.size(), 6);
    }

    Y_UNIT_TEST(TestDifferent)
    {
        TVector<TSkiffSchemaPtr> schemas;

        auto schema = CreateSimpleTypeSchema(EWireType::Uint64);
        schema->SetName("schema");
        schemas.push_back(schema);
        schemas.push_back(CreateSimpleTypeSchema(EWireType::Uint64));

        auto schema2 = CreateTupleSchema({
            CreateSimpleTypeSchema(EWireType::Int64),
            CreateSimpleTypeSchema(EWireType::String32),
        });
        schema2->SetName("s");
        schemas.push_back(schema2);

        auto schema3 = CreateTupleSchema({
            CreateSimpleTypeSchema(EWireType::Int64),
            CreateSimpleTypeSchema(EWireType::String32),
        });
        schema3->SetName("s0");
        schemas.push_back(schema3);

        auto schema4 = CreateRepeatedVariant16Schema({
            CreateSimpleTypeSchema(EWireType::Int64),
            schema2,
        });
        schemas.push_back(schema4);

        auto schema5 = CreateRepeatedVariant16Schema({
            CreateSimpleTypeSchema(EWireType::Int64),
            schema2,
        });
        schema5->SetName("kek");
        schemas.push_back(schema5);

        for (size_t i = 0; i < schemas.size(); ++i) {
            for (size_t j = i + 1; j < schemas.size(); ++j) {
                UNIT_ASSERT_VALUES_UNEQUAL(*schemas[i], *schemas[j]);
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////
