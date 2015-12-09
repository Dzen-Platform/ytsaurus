#include "framework.h"

#include <yt/core/misc/common.h>

#include <yt/core/ytree/attributes.h>
#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/yson_string.h>

namespace NYT {
namespace NYTree {
namespace {

////////////////////////////////////////////////////////////////////////////////

class TAttributesTest
    : public ::testing::Test
{
protected:
    virtual void SetUp()
    { }

    virtual void TearDown()
    { }
};

TEST(TAttributesTest, CheckAccessors)
{
    auto attributes = CreateEphemeralAttributes();
    attributes->Set<Stroka>("name", "Petr");
    attributes->Set<int>("age", 30);
    attributes->Set<double>("weight", 70.5);

    auto keys_ = attributes->List();
    yhash_set<Stroka> keys(keys_.begin(), keys_.end());
    yhash_set<Stroka> expectedKeys;
    expectedKeys.insert("name");
    expectedKeys.insert("age");
    expectedKeys.insert("weight");
    EXPECT_EQ(keys , expectedKeys);

    EXPECT_EQ("Petr", attributes->Get<Stroka>("name"));
    EXPECT_THROW(attributes->Get<int>("name"), std::exception);

    EXPECT_EQ(30, attributes->Find<int>("age"));
    EXPECT_EQ(30, attributes->Get<int>("age"));
    EXPECT_THROW(attributes->Get<char>("age"), std::exception);

    EXPECT_EQ(70.5, attributes->Get<double>("weight"));
    EXPECT_THROW(attributes->Get<Stroka>("weight"), std::exception);

    EXPECT_FALSE(attributes->Find<int>("unknown_key"));
    EXPECT_EQ(42, attributes->Get<int>("unknown_key", 42));
    EXPECT_THROW(attributes->Get<double>("unknown_key"), std::exception);
}

TEST(TAttributesTest, MergeFromTest)
{
    auto attributesX = CreateEphemeralAttributes();
    attributesX->Set<Stroka>("name", "Petr");
    attributesX->Set<int>("age", 30);

    auto attributesY = CreateEphemeralAttributes();
    attributesY->Set<Stroka>("name", "Oleg");

    attributesX->MergeFrom(*attributesY);
    EXPECT_EQ("Oleg", attributesX->Get<Stroka>("name"));
    EXPECT_EQ(30, attributesX->Get<int>("age"));

    auto node = ConvertToNode(TYsonString("{age=20}"));
    attributesX->MergeFrom(node->AsMap());
    EXPECT_EQ("Oleg", attributesX->Get<Stroka>("name"));
    EXPECT_EQ(20, attributesX->Get<int>("age"));
}

TEST(TAttributesTest, SerializeToNode)
{
    auto attributes = CreateEphemeralAttributes();
    attributes->Set<Stroka>("name", "Petr");
    attributes->Set<int>("age", 30);

    auto node = ConvertToNode(*attributes);
    auto convertedAttributes = ConvertToAttributes(node);
    EXPECT_EQ(*attributes, *convertedAttributes);
}

TEST(TAttributesTest, TrySerializeToProto)
{
    auto attributes = CreateEphemeralAttributes();
    attributes->Set<Stroka>("name", "Petr");
    attributes->Set<int>("age", 30);

    NProto::TAttributes protoAttributes;
    ToProto(&protoAttributes, *attributes);
    auto convertedAttributes = FromProto(protoAttributes);
    EXPECT_EQ(*attributes, *convertedAttributes);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYTree
} // namespace NYT
