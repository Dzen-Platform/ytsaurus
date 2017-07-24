#include "parser.h"
#include "consumer.h"
#include "format.h"
#include "parser_detail.h"

#include <yt/core/concurrency/coroutine.h>

#include <yt/core/misc/error.h>

namespace NYT {
namespace NYson {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TYsonParser::TImpl
{
private:
    typedef TCoroutine<int(const char* begin, const char* end, bool finish)> TParserCoroutine;

    TParserCoroutine ParserCoroutine_;

public:
    TImpl(
        IYsonConsumer* consumer,
        EYsonType parsingMode,
        bool enableLinePositionInfo,
        i64 memoryLimit,
        bool enableContext)
        : ParserCoroutine_(BIND(
            [=] (TParserCoroutine& self, const char* begin, const char* end, bool finish) {
                ParseYsonStreamImpl<IYsonConsumer, TBlockReader<TParserCoroutine>>(
                    TBlockReader<TParserCoroutine>(self, begin, end, finish),
                    consumer,
                    parsingMode,
                    enableLinePositionInfo,
                    memoryLimit,
                    enableContext);
            }))
    { }

    void Read(const char* begin, const char* end, bool finish = false)
    {
        if (!ParserCoroutine_.IsCompleted()) {
            ParserCoroutine_.Run(begin, end, finish);
        } else {
            THROW_ERROR_EXCEPTION("Input is already parsed");
        }
    }

    void Read(const TStringBuf& data, bool finish = false)
    {
        Read(data.begin(), data.end(), finish);
    }

    void Finish()
    {
        Read(0, 0, true);
    }
};

////////////////////////////////////////////////////////////////////////////////

TYsonParser::TYsonParser(
    IYsonConsumer* consumer,
    EYsonType type,
    bool enableLinePositionInfo,
    i64 memoryLimit,
    bool enableContext)
    : Impl(std::make_unique<TImpl>(
        consumer,
        type,
        enableLinePositionInfo,
        memoryLimit,
        enableContext))
{ }

TYsonParser::~TYsonParser()
{ }

void TYsonParser::Read(const TStringBuf& data)
{
    Impl->Read(data);
}

void TYsonParser::Finish()
{
    Impl->Finish();
}

////////////////////////////////////////////////////////////////////////////////

class TStatelessYsonParser::TImpl
{
private:
    const std::unique_ptr<TStatelessYsonParserImplBase> Impl;

public:
    TImpl(
        IYsonConsumer* consumer,
        bool enableLinePositionInfo,
        i64 memoryLimit,
        bool enableContext)
        : Impl([=] () -> TStatelessYsonParserImplBase* {
            if (enableContext && enableLinePositionInfo) {
                return new TStatelessYsonParserImpl<IYsonConsumer, 64, true>(consumer, memoryLimit);
            } else if (enableContext && !enableLinePositionInfo) {
                return new TStatelessYsonParserImpl<IYsonConsumer, 64, false>(consumer, memoryLimit);
            } else if (!enableContext && enableLinePositionInfo) {
                return new TStatelessYsonParserImpl<IYsonConsumer, 0, true>(consumer, memoryLimit);
            } else {
                return new TStatelessYsonParserImpl<IYsonConsumer, 0, false>(consumer, memoryLimit);
            }
        }())
    { }

    void Parse(const TStringBuf& data, EYsonType type = EYsonType::Node)
    {
        Impl->Parse(data, type);
    }
};

////////////////////////////////////////////////////////////////////////////////

TStatelessYsonParser::TStatelessYsonParser(
    IYsonConsumer* consumer,
    bool enableLinePositionInfo,
    i64 memoryLimit,
    bool enableContext)
    : Impl(new TImpl(consumer, enableLinePositionInfo, memoryLimit, enableContext))
{ }

TStatelessYsonParser::~TStatelessYsonParser()
{ }

void TStatelessYsonParser::Parse(const TStringBuf& data, EYsonType type)
{
    Impl->Parse(data, type);
}

////////////////////////////////////////////////////////////////////////////////

void ParseYsonStringBuffer(
    const TStringBuf& buffer,
    EYsonType type,
    IYsonConsumer* consumer,
    bool enableLinePositionInfo,
    i64 memoryLimit,
    bool enableContext)
{
    ParseYsonStreamImpl<IYsonConsumer, TStringReader>(
        TStringReader(buffer.begin(), buffer.end()),
        consumer,
        type,
        enableLinePositionInfo,
        memoryLimit,
        enableContext);
}

void ParseYsonSharedRefArray(
    const TSharedRefArray& refArray,
    EYsonType type,
    IYsonConsumer* consumer,
    bool enableLinePositionInfo,
    i64 memoryLimit,
    bool enableContext)
{
    typedef TCoroutine<int(const char* begin, const char* end, bool finish)> TParserCoroutine;
    TParserCoroutine parserCoroutine(BIND([=] (TParserCoroutine& self, const char* begin, const char* end, bool finish) {
        ParseYsonStreamImpl<IYsonConsumer, TBlockReader<TParserCoroutine>>(
            TBlockReader<TParserCoroutine>(self, begin, end, finish),
            consumer,
            type,
            enableLinePositionInfo,
            memoryLimit,
            enableContext);
    }));

    for (const auto& blob: refArray) {
        auto buffer = TStringBuf(blob.Begin(), blob.End());
        parserCoroutine.Run(buffer.begin(), buffer.end(), false);
    }

    parserCoroutine.Run(nullptr, nullptr, true);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
