#include "framework.h"

#include <yt/core/actions/future.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/subprocess.h>

namespace NYT {
namespace {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

#ifdef _unix_

TEST(TSubprocessTest, Basic)
{
    TSubprocess subprocess("/bin/bash");

    subprocess.AddArgument("-c");
    subprocess.AddArgument("true");

    auto result = subprocess.Execute();
    EXPECT_TRUE(result.Status.IsOK());
}


TEST(TSubprocessTest, PipeOutput)
{
    TSubprocess subprocess("/bin/echo");

    subprocess.AddArgument("hello");

    auto result = subprocess.Execute();
    EXPECT_TRUE(result.Status.IsOK());
    Stroka output(result.Output.Begin(), result.Output.End());
    EXPECT_TRUE(output == "hello\n") << output;
}

TEST(TSubprocessTest, PipeBigOutput)
{
    TActionQueuePtr queue = New<TActionQueue>();

    auto result = BIND([] () {
        TSubprocess subprocess("/bin/bash");

        subprocess.AddArgument("-c");
        subprocess.AddArgument("for i in `seq 100000`; do echo hello; done; echo world");

        auto result = subprocess.Execute();
        return result.Status.IsOK();
    }).AsyncVia(queue->GetInvoker()).Run().Get().Value();

    EXPECT_TRUE(result);
}


TEST(TSubprocessTest, PipeBigError)
{
    TActionQueuePtr queue = New<TActionQueue>();

    auto result = BIND([] () {
        TSubprocess subprocess("/bin/bash");

        subprocess.AddArgument("-c");
        subprocess.AddArgument("for i in `seq 100000`; do echo hello 1>&2; done; echo world");

        auto result = subprocess.Execute();
        return result;
    }).AsyncVia(queue->GetInvoker()).Run().Get().Value();

    EXPECT_TRUE(result.Status.IsOK());
    EXPECT_EQ(6*100000, result.Error.Size());
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
