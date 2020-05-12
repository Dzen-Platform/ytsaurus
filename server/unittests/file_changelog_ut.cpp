#include <yt/core/test_framework/framework.h>

#include <yt/server/lib/hydra/changelog.h>
#include <yt/server/lib/hydra/config.h>
#include <yt/server/lib/hydra/local_changelog_store.h>

#include <yt/ytlib/hydra/proto/hydra_manager.pb.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/misc/fs.h>

#include <yt/core/profiling/profiler.h>

#include <util/random/random.h>

#include <util/system/tempfile.h>

namespace NYT::NHydra {
namespace {

using namespace NConcurrency;
using namespace NHydra::NProto;

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogTest
    : public ::testing::Test
{
protected:
    TFileChangelogStoreConfigPtr ChangelogStoreConfig;
    IChangelogStoreFactoryPtr ChangelogStoreFactory;
    IChangelogStorePtr ChangelogStore;
    IChangelogPtr Changelog;

    TActionQueuePtr ActionQueue;
    IInvokerPtr Invoker;

    virtual void SetUp()
    {
        ChangelogStoreConfig = New<TFileChangelogStoreConfig>();
        ChangelogStoreConfig->Path = "FileChangelog";

        ChangelogStoreFactory = CreateLocalChangelogStoreFactory(
            ChangelogStoreConfig,
            "UTCLFlash",
            NProfiling::TProfiler());
        ChangelogStore = ChangelogStoreFactory->Lock()
            .Get()
            .ValueOrThrow();

        auto changelogOrError = ChangelogStore->CreateChangelog(0).Get();
        ASSERT_TRUE(changelogOrError.IsOK()) << ToString(changelogOrError);
        Changelog = changelogOrError.Value();

        ActionQueue = New<TActionQueue>();
        Invoker = ActionQueue->GetInvoker();
    }

    virtual void TearDown()
    {
        NFS::RemoveRecursive(ChangelogStoreConfig->Path);
    }
};

static void CheckRecord(i32 data, const TSharedRef& record)
{
    EXPECT_EQ(sizeof(data), record.Size());
    EXPECT_EQ(       data , *(reinterpret_cast<const i32*>(record.Begin())));
}

void ReadRecord(IChangelog* asyncChangeLog, i32 recordIndex)
{
    auto result = asyncChangeLog->Read(recordIndex, 1, std::numeric_limits<i64>::max())
        .Get()
        .ValueOrThrow();
    EXPECT_EQ(1, result.size());
    CheckRecord(recordIndex, result[0]);
}

TSharedRef MakeData(i32 data)
{
    auto result = TSharedMutableRef::Allocate(sizeof(i32));
    *reinterpret_cast<i32*>(&*result.Begin()) = static_cast<i32>(data);
    return result;
}

TEST_F(TFileChangelogTest, Empty)
{ }

TEST_F(TFileChangelogTest, ReadTrailingRecords)
{
    const int recordCount = 10000;
    TFuture<void> readResult;
    for (int recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
        auto flushResult = Changelog->Append({MakeData(recordIndex)});
        if (recordIndex % 1000 == 0) {
            flushResult.Get();
        }
        if (recordIndex % 10 == 0) {
            readResult = BIND(&ReadRecord, Unretained(Changelog.Get()), recordIndex).AsyncVia(Invoker).Run();
        }
    }
    readResult.Get();
}

TEST_F(TFileChangelogTest, ReadWithSizeLimit)
{
    for (int recordIndex = 0; recordIndex < 40; ++recordIndex) {
        Changelog->Append({MakeData(recordIndex)});
    }

    auto check = [&] (int maxSize) {
        auto records = Changelog->Read(0, 1000, maxSize)
            .Get()
            .ValueOrThrow();
        EXPECT_EQ((maxSize - 1) / sizeof(i32) + 1, records.size());
        for (int recordIndex = 0; recordIndex < static_cast<int>(records.size()); ++recordIndex) {
            CheckRecord(recordIndex, records[recordIndex]);
        }
    };

    check(1);
    check(10);
    check(40);
    check(100);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NMetaState
