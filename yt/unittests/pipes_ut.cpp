#include "stdafx.h"
#include "framework.h"

#include <core/concurrency/action_queue.h>
#include <core/concurrency/scheduler.h>

#include <core/misc/proc.h>

#include <core/pipes/io_dispatcher.h>
#include <core/pipes/async_reader.h>
#include <core/pipes/async_writer.h>

#include <random>

namespace NYT {
namespace NPipes {

////////////////////////////////////////////////////////////////////////////////

using namespace NConcurrency;

#ifndef _win_

void SafeMakeNonblockingPipes(int fds[2])
{
    SafePipe(fds);
    SafeMakeNonblocking(fds[0]);
    SafeMakeNonblocking(fds[1]);
}

TEST(TPipeIOHolder, CanInstantiate)
{
    int pipefds[2];
    SafeMakeNonblockingPipes(pipefds);

    auto readerHolder = New<TAsyncReader>(pipefds[0]);
    auto writerHolder = New<TAsyncWriter>(pipefds[1]);

    readerHolder->Abort().Get();
    writerHolder->Abort().Get();
}

//////////////////////////////////////////////////////////////////////////////////////////////////

TBlob ReadAll(TAsyncReaderPtr reader, bool useWaitFor)
{
    auto buffer = TBlob(TDefaultBlobTag(), 1024 * 1024);
    auto whole = TBlob(TDefaultBlobTag());

    while (true)  {
        TErrorOr<size_t> result;
        auto future = reader->Read(buffer.Begin(), buffer.Size());
        if (useWaitFor) {
            result = WaitFor(future);
        } else {
            result = future.Get();
        }

        if (result.ValueOrThrow() == 0) {
            break;
        }

        whole.Append(buffer.Begin(), result.Value());
    }
    return whole;
}

TEST(TAsyncWriterTest, AsyncCloseFail)
{
    int pipefds[2];
    SafeMakeNonblockingPipes(pipefds);

    auto reader = New<TAsyncReader>(pipefds[0]);
    auto writer = New<TAsyncWriter>(pipefds[1]);

    auto queue = New<NConcurrency::TActionQueue>();
    auto readFromPipe =
        BIND(&ReadAll, reader, false)
            .AsyncVia(queue->GetInvoker())
            .Run();

    std::vector<char> buffer(200*1024, 'a');

    auto writeResult = writer->Write(&buffer[0], buffer.size()).Get();

    EXPECT_TRUE(writeResult.IsOK())
        << ToString(writeResult);

    auto error = writer->Close();

    auto readResult = readFromPipe.Get();
    ASSERT_TRUE(readResult.IsOK())
        << ToString(readResult);

    auto closeStatus = error.Get();

    ASSERT_EQ(-1, close(pipefds[1]));
}

//////////////////////////////////////////////////////////////////////////////////////////////////

class TPipeReadWriteTest
    : public ::testing::Test
{
protected:
    virtual void SetUp() override
    {
        int pipefds[2];
        SafeMakeNonblockingPipes(pipefds);

        Reader = New<TAsyncReader>(pipefds[0]);
        Writer = New<TAsyncWriter>(pipefds[1]);
    }

    virtual void TearDown() override
    { }

    TAsyncReaderPtr Reader;
    TAsyncWriterPtr Writer;
};


TEST_F(TPipeReadWriteTest, ReadSomethingSpin)
{
    Stroka message("Hello pipe!\n");
    Writer->Write(message.c_str(), message.size()).Get();
    Writer->Close();

    auto data = TBlob(TDefaultBlobTag(), 1);
    auto whole = TBlob(TDefaultBlobTag());

    while (true)
    {
        auto result = Reader->Read(data.Begin(), data.Size()).Get();
        if (result.ValueOrThrow() == 0) {
            break;
        }
        whole.Append(data.Begin(), result.Value());
    }

    EXPECT_EQ(message, Stroka(whole.Begin(), whole.End()));
}

TEST_F(TPipeReadWriteTest, ReadSomethingWait)
{
    Stroka message("Hello pipe!\n");
    Writer->Write(message.c_str(), message.size()).Get();
    Writer->Close();

    auto whole = ReadAll(Reader, false);

    EXPECT_EQ(message, Stroka(whole.Begin(), whole.End()));
}

TEST_F(TPipeReadWriteTest, ReadWrite)
{
    Stroka text("Hello cruel world!\n");
    Writer->Write(text.c_str(), text.size()).Get();
    auto errorsOnClose = Writer->Close();

    auto textFromPipe = ReadAll(Reader, false);

    auto error = errorsOnClose.Get();
    EXPECT_TRUE(error.IsOK()) << error.GetMessage();
    EXPECT_EQ(text, Stroka(textFromPipe.Begin(), textFromPipe.End()));
}

void WriteAll(TAsyncWriterPtr writer, const char* data, size_t size, size_t blockSize)
{
    while (size > 0) {
        const size_t currentBlockSize = std::min(blockSize, size);
        auto error = WaitFor(writer->Write(data, currentBlockSize));
        THROW_ERROR_EXCEPTION_IF_FAILED(error);
        size -= currentBlockSize;
        data += currentBlockSize;
    }

    {
        auto error = WaitFor(writer->Close());
        THROW_ERROR_EXCEPTION_IF_FAILED(error);
    }
}

////////////////////////////////////////////////////////////////////////////////

class TPipeBigReadWriteTest
    : public TPipeReadWriteTest
    , public ::testing::WithParamInterface<std::pair<size_t, size_t>>
{ };

TEST_P(TPipeBigReadWriteTest, RealReadWrite)
{
    size_t dataSize, blockSize;
    std::tie(dataSize, blockSize) = GetParam();

    auto queue = New<NConcurrency::TActionQueue>();

    std::vector<char> data(dataSize, 'a');

    BIND([&] () {
        auto dice = std::bind(
            std::uniform_int_distribution<char>(0, 128),
            std::default_random_engine());
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = dice();
        }
    })
    .AsyncVia(queue->GetInvoker()).Run();

    auto writeError =  BIND(&WriteAll, Writer, data.data(), data.size(), blockSize)
        .AsyncVia(queue->GetInvoker())
        .Run();
    auto readFromPipe = BIND(&ReadAll, Reader, true)
        .AsyncVia(queue->GetInvoker())
        .Run();

    auto textFromPipe = readFromPipe.Get().ValueOrThrow();
    EXPECT_EQ(data.size(), textFromPipe.Size());
    auto result = std::mismatch(textFromPipe.Begin(), textFromPipe.End(), data.begin());
    EXPECT_TRUE(std::equal(textFromPipe.Begin(), textFromPipe.End(), data.begin())) <<
        (result.first - textFromPipe.Begin()) << " " << (int)(*result.first);
}

INSTANTIATE_TEST_CASE_P(
    ValueParametrized,
    TPipeBigReadWriteTest,
    ::testing::Values(
        std::make_pair(2000 * 4096, 4096),
        std::make_pair(100 * 4096, 10000),
        std::make_pair(100 * 4096, 100),
        std::make_pair(100, 4096)));

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
