#include <yt/core/test_framework/framework.h>

#include <yt/ytlib/ypath/rich.h>

#include <yt/core/yson/parser.h>
#include <yt/core/yson/writer.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/node.h>
#include <yt/core/ytree/tree_builder.h>
#include <yt/core/ytree/tree_visitor.h>
#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/ypath_service.h>

#include <util/string/vector.h>

namespace NYT {
namespace NYTree {
namespace {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

class TYPathTest
    : public ::testing::Test
{
public:
    IYPathServicePtr RootService;

    virtual void SetUp()
    {
        RootService = GetEphemeralNodeFactory()->CreateMap();
    }

    static TYsonString TextifyYson(const TYsonString& data)
    {
        return ConvertToYsonString(data, NYson::EYsonFormat::Text);
    }

    void Set(const TYPath& path, const TString& value)
    {
        SyncYPathSet(RootService, path, TYsonString(value));
    }

    void Remove(const TYPath& path)
    {
        SyncYPathRemove(RootService, path);
    }

    TYsonString Get(const TYPath& path)
    {
        return TextifyYson(SyncYPathGet(RootService, path));
    }

    std::vector<TString> List(const TYPath& path)
    {
        return SyncYPathList(RootService, path);
    }

    void Check(const TYPath& path, const TString& expected)
    {
        TYsonString output = Get(path);
        EXPECT_TRUE(
            AreNodesEqual(
                ConvertToNode(TYsonString(expected)),
                ConvertToNode(output)));
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TYPathTest, MapModification)
{
    Set("/map", "{hello=world; list=[0;a;{}]; n=1}");

    Set("/map/hello", "not_world");
    Check("", "{map={hello=not_world;list=[0;a;{}];n=1}}");

    Set("/map/list/2/some", "value");
    Check("", "{map={hello=not_world;list=[0;a;{some=value}];n=1}}");

    Remove("/map/n");
    Check("", "{map={hello=not_world;list=[0;a;{some=value}]}}");

    Set("/map/list", "[]");
    Check("", "{map={hello=not_world;list=[]}}");

    Remove("/map/hello");
    Check("", "{map={list=[]}}");

    Remove("/map");
    Check("", "{}");
}

TEST_F(TYPathTest, ListModification)
{
    Set("/list", "[1;2;3]");
    Check("", "{list=[1;2;3]}");
    Check("/list", "[1;2;3]");
    Check("/list/0", "1");
    Check("/list/1", "2");
    Check("/list/2", "3");
    Check("/list/-1", "3");
    Check("/list/-2", "2");
    Check("/list/-3", "1");

    Set("/list/end", "4");
    Check("/list", "[1;2;3;4]");

    Set("/list/end", "5");
    Check("/list", "[1;2;3;4;5]");

    Set("/list/2", "100");
    Check("/list", "[1;2;100;4;5]");

    Set("/list/-2", "3");
    Check("/list", "[1;2;100;3;5]");

    Remove("/list/4");
    Check("/list", "[1;2;100;3]");

    Remove("/list/2");
    Check("/list", "[1;2;3]");

    Remove("/list/-1");
    Check("/list", "[1;2]");

    Set("/list/before:0", "0");
    Check("/list", "[0;1;2]");

    Set("/list/after:1", "3");
    Check("/list", "[0;1;3;2]");

    Set("/list/after:-1", "4");
    Check("/list", "[0;1;3;2;4]");

    Set("/list/before:-1", "5");
    Check("/list", "[0;1;3;2;5;4]");

    Set("/list/begin", "6");
    Check("/list", "[6;0;1;3;2;5;4]");
}

TEST_F(TYPathTest, ListReassignment)
{
    Set("/list", "[a;b;c]");
    Set("/list", "[1;2;3]");

    Check("", "{list=[1;2;3]}");
}

TEST_F(TYPathTest, Clear)
{
    Set("/my", "{list=<type=list>[1;2];map=<type=map>{a=1;b=2}}");

    Remove("/my/list/*");
    Check("/my/list", "<type=list>[]");
    Check("/my/list/@", "{type=list}");

    Remove("/my/map/*");
    Check("/my/map", "<type=map>{}");
    Check("/my/map/@", "{type=map}");
}

TEST_F(TYPathTest, Ls)
{
    Set("", "{a={x1={y1=1}};b={x2={y2=2}};c={x3={y3=3}};d={x4={y4=4}}}");

    Remove("/b");
    Set("/e", "5");

    auto result = List("");
    std::sort(result.begin(), result.end());

    std::vector<TString> expected;
    expected.push_back("a");
    expected.push_back("c");
    expected.push_back("d");
    expected.push_back("e");

    EXPECT_EQ(expected, result);
}

TEST_F(TYPathTest, LsOnUnsupportedNodes)
{
    EXPECT_ANY_THROW({
        Set("list", "[1; 2; 3; 4]");
        List("list");
    });

    EXPECT_ANY_THROW({
        Set("str", "aaa");
        List("str");
    });

    EXPECT_ANY_THROW({
        Set("int", "42");
        List("int");
    });

    EXPECT_ANY_THROW({
        Set("double", "3.14");
        List("double");
    });

    EXPECT_ANY_THROW({
        Set("entity", "#");
        List("entity");
    });
}

TEST_F(TYPathTest, Attributes)
{
    Set("/root", "<attr=100;mode=rw> {nodes=[1; 2]}");
    Check("/root/@", "{attr=100;mode=rw}");
    Check("/root/@attr", "100");

    Set("/root/value", "<>500");
    Check("/root/value", "500");

    Remove("/root/@*");
    Check("/root/@", "{}");

    Remove("/root/nodes");
    Remove("/root/value");
    Check("", "{root={}}");

    Set("/root/2", "<author=ignat> #");
    Check("", "{root={\"2\"=<author=ignat>#}}");
    Check("/root/2/@", "{author=ignat}");
    Check("/root/2/@author", "ignat");

    // note: empty attributes are shown when nested
    Set("/root/3", "<dir=<file=<>-100>#>#");
    Check("/root/3/@", "{dir=<file=<>-100>#}");
    Check("/root/3/@dir/@", "{file=<>-100}");
    Check("/root/3/@dir/@file", "<>-100");
    Check("/root/3/@dir/@file/@", "{}");
}

TEST_F(TYPathTest, RemoveAll)
{
    // from map
    Set("/map", "{foo=bar;key=vaue}");
    Remove("/map/*");
    Check("/map", "{}");

    // from list
    Set("/list", "[10;20;30]");
    Remove("/list/*");
    Check("/list", "[]");

    // from attributes
    Set("/attr", "<foo=bar;key=vaue>42");
    Remove("/attr/@*");
    Check("/attr/@", "{}");
}

TEST_F(TYPathTest, InvalidCases)
{
    Set("/root", "{}");

    // exception when setting attributes
    EXPECT_ANY_THROW(Set("/root/some", "[10; {key=value;foo=<attr=42a>bar}]"));
    Check("/root", "{}");

    EXPECT_ANY_THROW(Set("/a/b", "1")); // /a must exist
    EXPECT_ANY_THROW(Set("a", "{}")); // must start with '/'
    EXPECT_ANY_THROW(Set("/root/", "{}")); // cannot end with '/'
    EXPECT_ANY_THROW(Set("", "[]")); // change the type of root
    EXPECT_ANY_THROW(Remove("")); // remove the root
    EXPECT_ANY_THROW(Get("/b")); // get non-existent path

    // get non-existent attribute from non-existent node
    EXPECT_ANY_THROW(Get("/b/@some"));

    // get non-existent attribute from existent node
    EXPECT_ANY_THROW({
       Set("/c", "{}");
       Get("/c/@some");
   });

    // remove non-existing child
    EXPECT_ANY_THROW(Remove("/a"));
}

TEST_F(TYPathTest, ParseRichYPath1)
{
    auto path = NYPath::TRichYPath::Parse("<a=b>//home/ignat{a,b}[1:2]");
    EXPECT_EQ(path.GetPath(), "//home/ignat");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(TYsonString("{a=b;columns=[a;b]; ranges=[{upper_limit={key=[2]};lower_limit={key=[1]}}]}"))));
}

TEST_F(TYPathTest, ParseRichYPath2)
{
    auto path = NYPath::TRichYPath::Parse("<a=b>//home");
    EXPECT_EQ(path.GetPath(), "//home");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(TYsonString("{a=b}"))));
}

TEST_F(TYPathTest, ParseRichYPath3)
{
    auto path = NYPath::TRichYPath::Parse("//home");
    EXPECT_EQ(path.GetPath(), "//home");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(EmptyAttributes())));
}

TEST_F(TYPathTest, ParseRichYPath4)
{
    auto path = NYPath::TRichYPath::Parse("//home[:]");
    EXPECT_EQ(path.GetPath(), "//home");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(TYsonString("{ranges=[{}]}"))));
}

TEST_F(TYPathTest, ParseRichYPath5)
{
    auto path = NYPath::TRichYPath::Parse("//home[(x, y):(a, b)]");
    EXPECT_EQ(path.GetPath(), "//home");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(TYsonString("{ranges=[{lower_limit={key=[x;y]};upper_limit={key=[a;b]}}]}"))));
}

TEST_F(TYPathTest, ParseRichYPath6)
{
    auto path = NYPath::TRichYPath::Parse("//home[#1:#2,x:y]");
    EXPECT_EQ(path.GetPath(), "//home");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(TYsonString(
                "{ranges=["
                    "{lower_limit={row_index=1};upper_limit={row_index=2}};"
                    "{lower_limit={key=[x]};upper_limit={key=[y]}}"
                "]}"))));
}

TEST_F(TYPathTest, ParseRichYPath7)
{
    auto path = NYPath::TRichYPath::Parse("//home[x:#1000]");
    EXPECT_EQ(path.GetPath(), "//home");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(TYsonString(
                "{ranges=["
                    "{lower_limit={key=[x]};upper_limit={row_index=1000}};"
                "]}"))));
}

TEST_F(TYPathTest, ParseRichYPath8)
{
    auto path = NYPath::TRichYPath::Parse(" <a=b> //home");
    EXPECT_EQ(path.GetPath(), "//home");
    EXPECT_TRUE(
        AreNodesEqual(
            ConvertToNode(path.Attributes()),
            ConvertToNode(TYsonString("{a=b}"))));
}

TEST_F(TYPathTest, ParseRichYPath9)
{
    auto path = NYPath::TRichYPath::Parse("@home");
    EXPECT_EQ(path.GetPath(), "@home");
}

TEST_F(TYPathTest, IgnoreAmpersand1)
{
    Set("&/a", "b");
    Check("/a", "b");
    Check("&/a", "b");
}

TEST_F(TYPathTest, IgnoreAmpersand2)
{
    Set("/list", "[]");
    Set("/list&/end", "0");
    Check("/list", "[0]");
}

TEST_F(TYPathTest, IgnoreAmpersand3)
{
    Set("/map", "{}");
    Set("/map/@attr", "value");
    Check("/map&/@attr", "value");
}

////////////////////////////////////////////////////////////////////////////////

class TEmbeddedYPathOpsTest
    : public ::testing::Test
{
public:
    static INodePtr ParseNode(const TString& data)
    {
        return ConvertToNode(TYsonString(data));
    }

    static void ExpectEqual(INodePtr node, const TString& ysonString)
    {
        EXPECT_EQ(ConvertToYsonString(node, EYsonFormat::Text).GetData(), ysonString);
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TEmbeddedYPathOpsTest, SimpleMap)
{
    const auto node = ParseNode(R""({foo={bar="baz"}})"");
    ExpectEqual(GetNodeByYPath(node, "/foo/bar"), R""("baz")"");

    EXPECT_THROW(GetNodeByYPath(node, "/foo/qux"), std::exception);
    EXPECT_THROW(GetNodeByYPath(node, "/foo/bar/qux"), std::exception);
}

TEST_F(TEmbeddedYPathOpsTest, SimpleList)
{
    const auto node = ParseNode(R""({home={roizner={list=[100; 500; {foo=bar}; 42]}}})"");
    ExpectEqual(GetNodeByYPath(node, "/home/roizner/list/1"), "500");
    ExpectEqual(GetNodeByYPath(node, "/home/roizner/list/-1"), "42");
    ExpectEqual(GetNodeByYPath(node, "/home/roizner/list/2/foo"), R""("bar")"");

    EXPECT_THROW(GetNodeByYPath(node, "/home/roizner/list/4"), std::exception);
    EXPECT_THROW(GetNodeByYPath(node, "/home/roizner/list/-5"), std::exception);
}

TEST_F(TEmbeddedYPathOpsTest, attributes)
{
    const auto node = ParseNode(R""({home=<account=sys>{dir1=<account=root;user_attr=<omg="embedded attributes">{foo=bar}>{};dir2={}}})"");
    ExpectEqual(GetNodeByYPath(node, "/home/dir1/@account"), R""("root")"");
    ExpectEqual(GetNodeByYPath(node, "/home/dir1/@user_attr/foo"), R""("bar")"");
    ExpectEqual(GetNodeByYPath(node, "/home/dir1/@user_attr/@omg"), R""("embedded attributes")"");

    EXPECT_THROW(GetNodeByYPath(node, "/home/dir1/@user_attr/bar"), std::exception);
    EXPECT_THROW(GetNodeByYPath(node, "/home/dir2/@account"), std::exception);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYTree
} // namespace NYT
