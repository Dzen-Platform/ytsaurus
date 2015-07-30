#pragma once

#include "detail.h"

namespace NYT {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

template <class TConsumer, class TBlockStream, bool EnableLinePositionInfo>
class TParser
    : public TLexerBase<TBlockStream, EnableLinePositionInfo>
{
private:
    using TBase = TLexerBase<TBlockStream, EnableLinePositionInfo>;
    TConsumer* Consumer;

public:
    TParser(const TBlockStream& blockStream, TConsumer* consumer, TMaybe<ui64> memoryLimit)
        : TBase(blockStream, memoryLimit)
        , Consumer(consumer)
    { }

    void DoParse(EYsonType ysonType)
    {
        switch (ysonType) {
            case YT_NODE:
                ParseNode<true>();
                break;

            case YT_LIST_FRAGMENT:
                ParseListFragment<true>(EndSymbol);
                break;

            case YT_MAP_FRAGMENT:
                ParseMapFragment<true>(EndSymbol);
                break;

            default:
                FAIL("unreachable");
        }

        while (!(TBase::IsFinished() && TBase::IsEmpty())) {
            if (TBase::template SkipSpaceAndGetChar<true>() != EndSymbol) {
                ythrow TYsonException() << Sprintf("Stray '%c' found", *TBase::Begin());
            } else if (!TBase::IsEmpty()) {
                TBase::Advance(1);
            }
        }
    }

    void ParseAttributes()
    {
        Consumer->OnBeginAttributes();
        ParseMapFragment(EndAttributesSymbol);
        TBase::SkipCharToken(EndAttributesSymbol);
        Consumer->OnEndAttributes();
    }

    void ParseMap()
    {
        Consumer->OnBeginMap();
        ParseMapFragment(EndMapSymbol);
        TBase::SkipCharToken(EndMapSymbol);
        Consumer->OnEndMap();
    }

    void ParseList()
    {
        Consumer->OnBeginList();
        ParseListFragment(EndListSymbol);
        TBase::SkipCharToken(EndListSymbol);
        Consumer->OnEndList();
    }

    template <bool AllowFinish>
    void ParseNode()
    {
        return ParseNode<AllowFinish>(TBase::SkipSpaceAndGetChar());
    }

    template <bool AllowFinish>
    void ParseNode(char ch)
    {
        if (ch == BeginAttributesSymbol) {
            TBase::Advance(1);
            ParseAttributes();
            ch = TBase::SkipSpaceAndGetChar();
        }

        switch (ch) {
            case BeginMapSymbol:
                TBase::Advance(1);
                ParseMap();
                break;

            case BeginListSymbol:
                TBase::Advance(1);
                ParseList();
                break;

            case '"': {
                TBase::Advance(1);
                TStringBuf value;
                TBase::ReadQuotedString(&value);
                Consumer->OnStringScalar(value);
                break;
            }
            case StringMarker: {
                TBase::Advance(1);
                TStringBuf value;
                TBase::ReadBinaryString(&value);
                Consumer->OnStringScalar(value);
                break;
            }
            case Int64Marker:{
                TBase::Advance(1);
                i64 value;
                TBase::ReadBinaryInt64(&value);
                Consumer->OnInt64Scalar(value);
                break;
            }
            case Uint64Marker:{
                TBase::Advance(1);
                ui64 value;
                TBase::ReadBinaryUint64(&value);
                Consumer->OnUint64Scalar(value);
                break;
            }
            case DoubleMarker: {
                TBase::Advance(1);
                double value;
                TBase::ReadBinaryDouble(&value);
                Consumer->OnDoubleScalar(value);
                break;
            }
            case FalseMarker: {
                TBase::Advance(1);
                Consumer->OnBooleanScalar(false);
                break;
            }
            case TrueMarker: {
                TBase::Advance(1);
                Consumer->OnBooleanScalar(true);
                break;
            }
            case EntitySymbol:
                TBase::Advance(1);
                Consumer->OnEntity();
                break;

            default: {
                if (isdigit(ch) || ch == '-' || ch == '+') { // case of '+' is handled in AfterPlus state
                    ReadNumeric<AllowFinish>();
                } else if (isalpha(ch) || ch == '_') {
                    TStringBuf value;
                    TBase::template ReadUnquotedString<AllowFinish>(&value);
                    Consumer->OnStringScalar(value);
                } else if (ch == '%') {
                    TBase::Advance(1);
                    Consumer->OnBooleanScalar(TBase::template ReadBoolean<AllowFinish>());
                } else {
                    ythrow TYsonException() << Sprintf("Unexpected '%c' while parsing node", ch);
                }
            }
        }
    }

    void ParseKey()
    {
        return ParseKey(TBase::SkipSpaceAndGetChar());
    }

    void ParseKey(char ch)
    {
        switch (ch) {
            case '"': {
                TBase::Advance(1);
                TStringBuf value;
                TBase::ReadQuotedString(&value);
                Consumer->OnKeyedItem(value);
                break;
            }
            case StringMarker: {
                TBase::Advance(1);
                TStringBuf value;
                TBase::ReadBinaryString(&value);
                Consumer->OnKeyedItem(value);
                break;
            }
            default: {
                if (isalpha(ch) || ch == '_') {
                    TStringBuf value;
                    TBase::ReadUnquotedString(&value);
                    Consumer->OnKeyedItem(value);
                } else {
                    ythrow TYsonException() << Sprintf("Unexpected '%c' while parsing key", ch);
                }
            }
        }
    }

    template <bool AllowFinish>
    void ParseMapFragment(char endSymbol)
    {
        char ch = TBase::template SkipSpaceAndGetChar<AllowFinish>();
        while (ch != endSymbol) {
            ParseKey(ch);
            ch = TBase::template SkipSpaceAndGetChar<AllowFinish>();
            if (ch == KeyValueSeparatorSymbol) {
                TBase::Advance(1);
            } else {
                ythrow TYsonException() << Sprintf("Expected '%c' but '%c' found", KeyValueSeparatorSymbol, ch);
            }
            ParseNode<AllowFinish>();
            ch = TBase::template SkipSpaceAndGetChar<AllowFinish>();
            if (ch == KeyedItemSeparatorSymbol) {
                TBase::Advance(1);
                ch = TBase::template SkipSpaceAndGetChar<AllowFinish>();
            } else if (ch != endSymbol) {
                ythrow TYsonException() << Sprintf("Expected '%c' or '%c' but '%c' found",
                    KeyedItemSeparatorSymbol, endSymbol, ch);
            }

        }
    }

    void ParseMapFragment(char endSymbol)
    {
        ParseMapFragment<false>(endSymbol);
    }

    template <bool AllowFinish>
    void ParseListFragment(char endSymbol)
    {
        char ch = TBase::template SkipSpaceAndGetChar<AllowFinish>();
        while (ch != endSymbol) {
            Consumer->OnListItem();
            ParseNode<AllowFinish>(ch);
            ch = TBase::template SkipSpaceAndGetChar<AllowFinish>();
            if (ch == ListItemSeparatorSymbol) {
                TBase::Advance(1);
                ch = TBase::template SkipSpaceAndGetChar<AllowFinish>();
            } else if (ch != endSymbol) {
                ythrow TYsonException() << Sprintf("Expected '%c' or '%c' but '%c' found",
                    ListItemSeparatorSymbol, endSymbol, ch);
            }
        }
    }

    void ParseListFragment(char endSymbol)
    {
        ParseListFragment<false>(endSymbol);
    }

    template <bool AllowFinish>
    void ReadNumeric()
    {
        TStringBuf valueBuffer;
        ENumericResult numericResult = TBase::template ReadNumeric<AllowFinish>(&valueBuffer);

        if (numericResult == ENumericResult::Double) {
            double value;
            try {
                value = FromString<double>(valueBuffer);
            } catch (yexception& e) {
                // This exception is wrapped in parser.
                ythrow TYsonException() << Sprintf("Failed to parse double literal '%s'", ~Stroka(valueBuffer)) << e;
            }
            Consumer->OnDoubleScalar(value);
        } else if (numericResult == ENumericResult::Int64) {
            i64 value;
            try {
                value = FromString<i64>(valueBuffer);
            } catch (yexception& e) {
                // This exception is wrapped in parser.
                ythrow TYsonException() << Sprintf("Failed to parse int64 literal '%s'", ~Stroka(valueBuffer)) << e;
            }
            Consumer->OnInt64Scalar(value);
        } else if (numericResult == ENumericResult::Uint64) {
            ui64 value;
            try {
                value = FromString<ui64>(valueBuffer.SubStr(0, valueBuffer.size() - 1));
            } catch (yexception& e) {
                // This exception is wrapped in parser.
                ythrow TYsonException() << Sprintf("Failed to parse uint64 literal '%s'", ~Stroka(valueBuffer)) << e;
            }
            Consumer->OnUint64Scalar(value);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

template <class TConsumer, class TBlockStream>
void ParseYsonStreamImpl(
    const TBlockStream& blockStream,
    IYsonConsumer* consumer,
    EYsonType parsingMode,
    bool enableLinePositionInfo,
    TMaybe<ui64> memoryLimit)
{
    if (enableLinePositionInfo) {
        using TImpl = NDetail::TParser<TConsumer, TBlockStream, true>;
        TImpl impl(blockStream, consumer, memoryLimit);
        impl.DoParse(parsingMode);
    } else {
        using TImpl = NDetail::TParser<TConsumer, TBlockStream, false>;
        TImpl impl(blockStream, consumer, memoryLimit);
        impl.DoParse(parsingMode);
    }
}

class TStatelessYsonParserImplBase
{
public:
    virtual void Parse(const TStringBuf& data, EYsonType type = YT_NODE) = 0;

    virtual ~TStatelessYsonParserImplBase()
    { }
};

template <class TConsumer, bool EnableLinePositionInfo>
class TStatelessYsonParserImpl
    : public TStatelessYsonParserImplBase
{
private:
    using TParser = NDetail::TParser<TConsumer, TStringReader, EnableLinePositionInfo>;
    TParser Parser;

public:
    TStatelessYsonParserImpl(TConsumer* consumer, TMaybe<ui64> memoryLimit)
        : Parser(TStringReader(), consumer, memoryLimit)
    { }

    void Parse(const TStringBuf& data, EYsonType type = YT_NODE) override
    {
        Parser.SetBuffer(data.begin(), data.end());
        Parser.DoParse(type);
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
