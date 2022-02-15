#include <yt/yt/core/test_framework/framework.h>

#include "chunk_helpers.h"

#include <yt/yt/server/master/chunk_server/chunk.h>
#include <yt/yt/server/master/chunk_server/chunk_list.h>
#include <yt/yt/server/master/chunk_server/helpers.h>

#include <random>

namespace NYT::NChunkServer {
namespace {

using namespace NTesting;

////////////////////////////////////////////////////////////////////////////////

class THunkChunkListTest
    : public TChunkGeneratorBase
{ };

TEST_F(THunkChunkListTest, HunkRootChildTracking)
{
    auto* root = CreateChunkList(EChunkListKind::SortedDynamicTablet);
    EXPECT_EQ(nullptr, root->GetHunkRootChild());

    auto* child1 = CreateChunkList(EChunkListKind::SortedDynamicSubtablet);
    auto* child2 = CreateChunkList(EChunkListKind::SortedDynamicSubtablet);
    auto* child3 = CreateChunk(4, 3, 2, 1);
    auto* hunkRootChild = CreateChunkList(EChunkListKind::HunkRoot);

    AttachToChunkList(root, {
        child1,
        child2
    });

    EXPECT_EQ(nullptr, root->GetHunkRootChild());

    AttachToChunkList(root, {
        hunkRootChild
    });
    EXPECT_EQ(hunkRootChild, root->GetHunkRootChild());

    AttachToChunkList(root, {
        child3
    });
    EXPECT_EQ(hunkRootChild, root->GetHunkRootChild());

    DetachFromChunkList(
        root,
        { child1 },
        EChunkDetachPolicy::SortedTablet);
    EXPECT_EQ(hunkRootChild, root->GetHunkRootChild());

    DetachFromChunkList(
        root,
        { hunkRootChild },
        EChunkDetachPolicy::SortedTablet);
    EXPECT_EQ(nullptr, root->GetHunkRootChild());

    DetachFromChunkList(
        root,
        { child2, child3 },
        EChunkDetachPolicy::SortedTablet);
    EXPECT_EQ(nullptr, root->GetHunkRootChild());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NChunkServer
