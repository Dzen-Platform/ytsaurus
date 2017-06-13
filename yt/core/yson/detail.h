#pragma once

#include "public.h"

#include <yt/core/concurrency/coroutine.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/zigzag.h>
#include <yt/core/misc/parser_helpers.h>

#include <util/generic/string.h>

#include <util/string/escape.h>

namespace NYT {
namespace NYson {

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {
/*! \internal */
////////////////////////////////////////////////////////////////////////////////

//! Indicates the beginning of a list.
const char BeginListSymbol = '[';
//! Indicates the end of a list.
const char EndListSymbol = ']';

//! Indicates the beginning of a map.
const char BeginMapSymbol = '{';
//! Indicates the end of a map.
const char EndMapSymbol = '}';

//! Indicates the beginning of an attribute map.
const char BeginAttributesSymbol = '<';
//! Indicates the end of an attribute map.
const char EndAttributesSymbol = '>';

//! Separates items in lists, maps, attributes.
const char ItemSeparatorSymbol = ';';
//! Separates keys from values in maps.
const char KeyValueSeparatorSymbol = '=';

//! Indicates an entity.
const char EntitySymbol = '#';

//! Indicates end of stream.
const char EndSymbol = '\0';

//! Marks the beginning of a binary string literal.
const char StringMarker = '\x01';
//! Marks the beginning of a binary i64 literal.
const char Int64Marker = '\x02';
//! Marks the beginning of a binary double literal.
const char DoubleMarker = '\x03';
//! Marks true and false values of boolean.
const char FalseMarker = '\x04';
const char TrueMarker = '\x05';
//! Marks the beginning of a binary ui64 literal.
const char Uint64Marker = '\x06';

template <bool EnableLinePositionInfo>
class TPositionInfo;

template <>
class TPositionInfo<true>
{
private:
    i64 Offset;
    int Line;
    int Column;

public:
    TPositionInfo()
        : Offset(0)
        , Line(1)
        , Column(1)
    { }

    void OnRangeConsumed(const char* begin, const char* end)
    {
        Offset += end - begin;
        for (auto current = begin; current != end; ++current) {
            ++Column;
            if (*current == '\n') { //TODO: memchr
                ++Line;
                Column = 1;
            }
        }
    }

    friend TError operator << (TError error, const TPositionInfo<true>& info)
    {
        return error
            << TErrorAttribute("offset", info.Offset)
            << TErrorAttribute("line", info.Line)
            << TErrorAttribute("column", info.Column);
    }
};

template <>
class TPositionInfo<false>
{
private:
    i64 Offset;

public:
    TPositionInfo()
        : Offset(0)
    { }

    void OnRangeConsumed(const char* begin, const char* end)
    {
        Offset += end - begin;
    }

    friend TError operator << (TError error, const TPositionInfo<false>& info)
    {
        return error
            << TErrorAttribute("offset", info.Offset);
    }
};

template <class TBlockStream, size_t MaxContextSize>
class TReaderWithContext
    : public TBlockStream
{
private:
    // ContextBegin points to the first byte of current buffer we want to append to context.
    // We set ContextBegin to nullptr when we want to write context but we haven't see any data yet,
    // we'll save context from the first data we'll see.
    // We set ContextBegin to Null when we don't' write context currently (or saved contexts already reached maximum size).
    TNullable<const char*> ContextBegin;
    size_t ContextSize = 0;
    char Context[MaxContextSize];

public:
    TReaderWithContext(const TBlockStream& blockStream)
        : TBlockStream(blockStream)
    { }

    void CheckpointContext()
    {
        ContextBegin = TBlockStream::Begin();
        ContextSize = 0;
    }

    TString GetContextFromCheckpoint() const
    {
        TString result;
        result.append(Context, ContextSize);
        if (ContextBegin && *ContextBegin != nullptr) {
            size_t remainingSize = MaxContextSize - ContextSize;
            remainingSize = std::min<size_t>(remainingSize, TBlockStream::End() - *ContextBegin);
            result.append(*ContextBegin, *ContextBegin + remainingSize);
        }
        return result;
    }

    void RefreshBlock()
    {
        if (ContextBegin) {
            // ContextBegin can be defined but set to nullptr if someone called CheckpointContext
            // before any data was read by TBlockStream (when both Begin()/End() == nullptr).
            if (*ContextBegin != nullptr) {
                const auto sizeToCopy = std::min<size_t>(MaxContextSize - ContextSize, TBlockStream::End() - *ContextBegin);
                memcpy(Context + ContextSize, *ContextBegin, sizeToCopy);
                ContextSize += sizeToCopy;
            }

            TBlockStream::RefreshBlock();

            // If we reached maximum size set ContextBegin to Null (not nullptr).
            // By doing this we disable further context saving.
            // Otherwise our context is continued from the beginning of the next block.
            if (ContextSize == MaxContextSize) {
                ContextBegin = Null;
            } else {
                ContextBegin = TBlockStream::Begin();
            }
        } else {
            TBlockStream::RefreshBlock();
        }
    }
};

template <class TBlockStream>
class TReaderWithContext<TBlockStream, 0>
    : public TBlockStream
{
public:
    TReaderWithContext(const TBlockStream& blockStream)
        : TBlockStream(blockStream)
    { }

    void CheckpointContext()
    { }

    TString GetContextFromCheckpoint() const
    {
        return "<context is disabled>";
    }
};

template <class TBlockStream, class TPositionBase>
class TCharStream
    : public TBlockStream
    , public TPositionBase
{
public:
    TCharStream(const TBlockStream& blockStream)
        : TBlockStream(blockStream)
    { }

    bool IsEmpty() const
    {
        return TBlockStream::Begin() == TBlockStream::End();
    }

    template <bool AllowFinish>
    void Refresh()
    {
        while (IsEmpty() && !TBlockStream::IsFinished()) {
            TBlockStream::RefreshBlock();
        }
        if (IsEmpty() && TBlockStream::IsFinished() && !AllowFinish) {
            THROW_ERROR_EXCEPTION("Premature end of stream")
                << *this;
        }
    }

    void Refresh()
    {
        return Refresh<false>();
    }

    template <bool AllowFinish>
    char GetChar()
    {
        Refresh<AllowFinish>();
        return !IsEmpty() ? *TBlockStream::Begin() : '\0';
    }

    char GetChar()
    {
        return GetChar<false>();
    }

    void Advance(size_t bytes)
    {
        TPositionBase::OnRangeConsumed(TBlockStream::Begin(), TBlockStream::Begin() + bytes);
        TBlockStream::Advance(bytes);
    }

    size_t Length() const
    {
        return TBlockStream::End() - TBlockStream::Begin();
    }
};

template <class TBaseStream>
class TCodedStream
    : public TBaseStream
{
private:
    static const int MaxVarintBytes = 10;
    static const int MaxVarint32Bytes = 5;

    const ui8* BeginByte() const
    {
        return reinterpret_cast<const ui8*>(TBaseStream::Begin());
    }

    const ui8* EndByte() const
    {
        return reinterpret_cast<const ui8*>(TBaseStream::End());
    }

    // Following functions is an adaptation Protobuf code from coded_stream.cc
    bool ReadVarint32FromArray(ui32* value)
    {
        // Fast path:  We have enough bytes left in the buffer to guarantee that
        // this read won't cross the end, so we can skip the checks.
        const ui8* ptr = BeginByte();
        ui32 b;
        ui32 result;

        b = *(ptr++); result  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
        b = *(ptr++); result |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
        b = *(ptr++); result |= (b & 0x7F) << 14; if (!(b & 0x80)) goto done;
        b = *(ptr++); result |= (b & 0x7F) << 21; if (!(b & 0x80)) goto done;
        b = *(ptr++); result |=  b         << 28; if (!(b & 0x80)) goto done;

        // If the input is larger than 32 bits, we still need to read it all
        // and discard the high-order bits.

        for (int i = 0; i < MaxVarintBytes - MaxVarint32Bytes; i++) {
            b = *(ptr++); if (!(b & 0x80)) goto done;
        }

        // We have overrun the maximum size of a Varint (10 bytes).  Assume
        // the data is corrupt.
        return false;

    done:
        TBaseStream::Advance(ptr - BeginByte());
        *value = result;
        return true;
    }

    bool ReadVarint32Fallback(ui32* value)
    {
        if (BeginByte() + MaxVarintBytes <= EndByte() ||
            // Optimization:  If the Varint ends at exactly the end of the buffer,
            // we can detect that and still use the fast path.
            (BeginByte() < EndByte() && !(EndByte()[-1] & 0x80)))
        {
            return ReadVarint32FromArray(value);
        } else {
            // Really slow case: we will incur the cost of an extra function call here,
            // but moving this out of line reduces the size of this function, which
            // improves the common case. In micro benchmarks, this is worth about 10-15%
            return ReadVarint32Slow(value);
        }
    }

    bool ReadVarint32Slow(ui32* value)
    {
        ui64 result;
        // Directly invoke ReadVarint64Fallback, since we already tried to optimize
        // for one-byte Varints.
        if (ReadVarint64Fallback(&result)) {
            *value = static_cast<ui32>(result);
            return true;
        } else {
            return false;
        }
    }

    bool ReadVarint64Slow(ui64* value)
    {
        // Slow path:  This read might cross the end of the buffer, so we
        // need to check and refresh the buffer if and when it does.

        ui64 result = 0;
        int count = 0;
        ui32 b;

        do {
            if (count == MaxVarintBytes) {
                return false;
            }
            while (BeginByte() == EndByte()) {
                TBaseStream::Refresh();
            }
            b = *BeginByte();
            result |= static_cast<ui64>(b & 0x7F) << (7 * count);
            TBaseStream::Advance(1);
            ++count;
        } while (b & 0x80);

        *value = result;
        return true;
    }

    bool ReadVarint64Fallback(ui64* value)
    {
        if (BeginByte() + MaxVarintBytes <= EndByte() ||
            // Optimization:  If the Varint ends at exactly the end of the buffer,
            // we can detect that and still use the fast path.
            (BeginByte() < EndByte() && !(EndByte()[-1] & 0x80)))
        {
            // Fast path:  We have enough bytes left in the buffer to guarantee that
            // this read won't cross the end, so we can skip the checks.

            const ui8* ptr = BeginByte();
            ui32 b;

            // Splitting into 32-bit pieces gives better performance on 32-bit
            // processors.
            ui32 part0 = 0, part1 = 0, part2 = 0;

            b = *(ptr++); part0  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
            b = *(ptr++); part0 |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
            b = *(ptr++); part0 |= (b & 0x7F) << 14; if (!(b & 0x80)) goto done;
            b = *(ptr++); part0 |= (b & 0x7F) << 21; if (!(b & 0x80)) goto done;
            b = *(ptr++); part1  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
            b = *(ptr++); part1 |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
            b = *(ptr++); part1 |= (b & 0x7F) << 14; if (!(b & 0x80)) goto done;
            b = *(ptr++); part1 |= (b & 0x7F) << 21; if (!(b & 0x80)) goto done;
            b = *(ptr++); part2  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
            b = *(ptr++); part2 |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;

            // We have overrun the maximum size of a Varint (10 bytes).  The data
            // must be corrupt.
            return false;

        done:
            TBaseStream::Advance(ptr - BeginByte());
            *value = (static_cast<ui64>(part0)      ) |
                        (static_cast<ui64>(part1) << 28) |
                        (static_cast<ui64>(part2) << 56);
            return true;
        } else {
            return ReadVarint64Slow(value);
        }
    }

public:
    TCodedStream(const TBaseStream& baseStream)
        : TBaseStream(baseStream)
    { }

    bool ReadVarint64(ui64* value)
    {
        if (BeginByte() < EndByte() && *BeginByte() < 0x80) {
            *value = *BeginByte();
            TBaseStream::Advance(1);
            return true;
        } else {
            return ReadVarint64Fallback(value);
        }
    }

    bool ReadVarint32(ui32* value)
    {
        if (BeginByte() < EndByte() && *BeginByte() < 0x80) {
            *value = *BeginByte();
            TBaseStream::Advance(1);
            return true;
        } else {
            return ReadVarint32Fallback(value);
        }
    }
};

DEFINE_ENUM(ENumericResult,
    ((Int64)                 (0))
    ((Uint64)                (1))
    ((Double)                (2))
);

template <class TBlockStream, bool EnableLinePositionInfo>
class TLexerBase
    : public TCodedStream<TCharStream<TBlockStream, TPositionInfo<EnableLinePositionInfo>>>
{
private:
    typedef TCodedStream<TCharStream<TBlockStream, TPositionInfo<EnableLinePositionInfo>>> TBaseStream;

    std::vector<char> Buffer_;
    const size_t MemoryLimit_;

    void Insert(const char* begin, const char* end)
    {
        ReserverAndCheckMemoryLimit(end - begin);
        Buffer_.insert(Buffer_.end(), begin, end);
    }

    void PushBack(char ch)
    {
        ReserverAndCheckMemoryLimit(1);
        Buffer_.push_back(ch);
    }

    void ReserverAndCheckMemoryLimit(size_t size)
    {
        auto minReserveSize = Buffer_.size() + size;
        if (minReserveSize <= Buffer_.capacity()) {
            return;
        }

        auto newDefaultCapacity = std::max(Buffer_.capacity(), size_t(1)) * 2;

        if (MemoryLimit_) {
            if (minReserveSize > MemoryLimit_) {
                THROW_ERROR_EXCEPTION(
                    "Memory limit exceeded while parsing YSON stream: allocated %v, limit %v",
                    minReserveSize,
                    MemoryLimit_);
            }

            newDefaultCapacity = std::min(newDefaultCapacity, MemoryLimit_);
        }

        auto reserveSize = std::max(newDefaultCapacity, minReserveSize);

        Buffer_.reserve(reserveSize);
    }

public:
    TLexerBase(
        const TBlockStream& blockStream,
        i64 memoryLimit = std::numeric_limits<i64>::max())
        : TBaseStream(blockStream)
        , MemoryLimit_(memoryLimit)
    { }

protected:
    /// Lexer routines

    template <bool AllowFinish>
    ENumericResult ReadNumeric(TStringBuf* value)
    {
        Buffer_.clear();
        auto result = ENumericResult::Int64;
        while (true) {
            char ch = TBaseStream::template GetChar<AllowFinish>();
            if (isdigit(ch) || ch == '+' || ch == '-') { // Seems like it can't be '+' or '-'
                PushBack(ch);
            } else if (ch == '.' || ch == 'e' || ch == 'E') {
                PushBack(ch);
                result = ENumericResult::Double;
            } else if (ch == 'u') {
                PushBack(ch);
                result = ENumericResult::Uint64;
            } else if (isalpha(ch)) {
                THROW_ERROR_EXCEPTION("Unexpected %Qv in numeric literal",
                    ch)
                    << *this;
            } else {
                break;
            }
            TBaseStream::Advance(1);
        }

        *value = TStringBuf(Buffer_.data(), Buffer_.size());
        return result;
    }

    void ReadQuotedString(TStringBuf* value)
    {
        Buffer_.clear();
        while (true) {
            if (TBaseStream::IsEmpty()) {
                TBaseStream::Refresh();
            }
            char ch = *TBaseStream::Begin();
            TBaseStream::Advance(1);
            if (ch != '"') {
                PushBack(ch);
            } else {
                // We must count the number of '\' at the end of StringValue
                // to check if it's not \"
                int slashCount = 0;
                int length = Buffer_.size();
                while (slashCount < length && Buffer_[length - 1 - slashCount] == '\\') {
                    ++slashCount;
                }
                if (slashCount % 2 == 0) {
                    break;
                } else {
                    PushBack(ch);
                }
            }
        }

        auto unquotedValue = UnescapeC(Buffer_.data(), Buffer_.size());
        Buffer_.clear();
        Insert(unquotedValue.data(), unquotedValue.data() + unquotedValue.size());
        *value = TStringBuf(Buffer_.data(), Buffer_.size());
    }

    template <bool AllowFinish>
    void ReadUnquotedString(TStringBuf* value)
    {
        Buffer_.clear();
        while (true) {
            char ch = TBaseStream::template GetChar<AllowFinish>();
            if (isalpha(ch) || isdigit(ch) ||
                ch == '_' || ch == '-' || ch == '%' || ch == '.')
            {
                PushBack(ch);
            } else {
                break;
            }
            TBaseStream::Advance(1);
        }
        *value = TStringBuf(Buffer_.data(), Buffer_.size());
    }

    void ReadUnquotedString(TStringBuf* value)
    {
        return ReadUnquotedString<false>(value);
    }

    void ReadBinaryString(TStringBuf* value)
    {
        ui32 ulength = 0;
        if (!TBaseStream::ReadVarint32(&ulength)) {
            THROW_ERROR_EXCEPTION("Error parsing varint value")
                << *this;
        }

        i32 length = ZigZagDecode32(ulength);
        if (length < 0) {
            THROW_ERROR_EXCEPTION("Negative binary string literal length %v",
                length)
                << *this;
        }

        if (TBaseStream::Begin() + length <= TBaseStream::End()) {
            *value = TStringBuf(TBaseStream::Begin(), length);
            TBaseStream::Advance(length);
        } else {
            size_t needToRead = length;
            Buffer_.clear();
            while (needToRead > 0) {
                if (TBaseStream::IsEmpty()) {
                    TBaseStream::Refresh();
                    continue;
                }
                size_t readingBytes = std::min(needToRead, TBaseStream::Length());
                Insert(TBaseStream::Begin(), TBaseStream::Begin() + readingBytes);
                needToRead -= readingBytes;
                TBaseStream::Advance(readingBytes);
            }
            *value = TStringBuf(Buffer_.data(), Buffer_.size());
        }
    }

    template <bool AllowFinish>
    bool ReadBoolean()
    {
        Buffer_.clear();

        static const auto trueString = STRINGBUF("true");
        static const auto falseString = STRINGBUF("false");

        auto throwIncorrectBoolean = [&] () {
            THROW_ERROR_EXCEPTION("Incorrect boolean string %Qv",
                TStringBuf(Buffer_.data(), Buffer_.size()));
        };

        PushBack(TBaseStream::template GetChar<AllowFinish>());
        TBaseStream::Advance(1);
        if (Buffer_[0] == trueString[0]) {
            for (int i = 1; i < trueString.Size(); ++i) {
                PushBack(TBaseStream::template GetChar<AllowFinish>());
                TBaseStream::Advance(1);
                if (Buffer_.back() != trueString[i]) {
                    throwIncorrectBoolean();
                }
            }
            return true;
        } else if (Buffer_[0] == falseString[0]) {
            for (int i = 1; i < falseString.Size(); ++i) {
                PushBack(TBaseStream::template GetChar<AllowFinish>());
                TBaseStream::Advance(1);
                if (Buffer_.back() != falseString[i]) {
                    throwIncorrectBoolean();
                }
            }
            return false;
        } else {
            throwIncorrectBoolean();
        }

        Y_UNREACHABLE();
    }

    void ReadBinaryInt64(i64* result)
    {
        ui64 uvalue;
        if (!TBaseStream::ReadVarint64(&uvalue)) {
            THROW_ERROR_EXCEPTION("Error parsing varint value")
                << *this;
        }
        *result = ZigZagDecode64(uvalue);
    }

    void ReadBinaryUint64(ui64* result)
    {
        ui64 uvalue;
        if (!TBaseStream::ReadVarint64(&uvalue)) {
            THROW_ERROR_EXCEPTION("Error parsing varint value")
                << *this;
        }
        *result = uvalue;
    }

    void ReadBinaryDouble(double* value)
    {
        size_t needToRead = sizeof(double);

        while (needToRead != 0) {
            if (TBaseStream::IsEmpty()) {
                TBaseStream::Refresh();
                continue;
            }

            size_t chunkSize = std::min(needToRead, TBaseStream::Length());
            if (chunkSize == 0) {
                THROW_ERROR_EXCEPTION("Error parsing binary double literal")
                    << *this;
            }
            std::copy(
                TBaseStream::Begin(),
                TBaseStream::Begin() + chunkSize,
                reinterpret_cast<char*>(value) + (sizeof(double) - needToRead));
            needToRead -= chunkSize;
            TBaseStream::Advance(chunkSize);
        }
    }

    /// Helpers
    void SkipCharToken(char symbol)
    {
        char ch = SkipSpaceAndGetChar();
        if (ch != symbol) {
            THROW_ERROR_EXCEPTION("Expected %Qv but found %Qv",
                symbol,
                ch)
                << *this;
        }

        TBaseStream::Advance(1);
    }

    template <bool AllowFinish>
    char SkipSpaceAndGetChar()
    {
        if (!TBaseStream::IsEmpty()) {
            char ch = *TBaseStream::Begin();
            if (!IsSpace(ch)) {
                return ch;
            }
        }
        return SkipSpaceAndGetCharFallback<AllowFinish>();
    }

    char SkipSpaceAndGetChar()
    {
        return SkipSpaceAndGetChar<false>();
    }

    template <bool AllowFinish>
    char SkipSpaceAndGetCharFallback()
    {
        while (true) {
            if (TBaseStream::IsEmpty()) {
                if (TBaseStream::IsFinished()) {
                    return '\0';
                }
                TBaseStream::template Refresh<AllowFinish>();
                continue;
            }
            if (!IsSpace(*TBaseStream::Begin())) {
                break;
            }
            TBaseStream::Advance(1);
        }
        return TBaseStream::template GetChar<AllowFinish>();
    }
};
////////////////////////////////////////////////////////////////////////////////
/*! \endinternal */
} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

class TStringReader
{
private:
    const char* BeginPtr;
    const char* EndPtr;

public:
    TStringReader()
        : BeginPtr(0)
        , EndPtr(0)
    { }

    TStringReader(const char* begin, const char* end)
        : BeginPtr(begin)
        , EndPtr(end)
    { }

    const char* Begin() const
    {
        return BeginPtr;
    }

    const char* End() const
    {
        return EndPtr;
    }

    void RefreshBlock()
    {
        Y_UNREACHABLE();
    }

    void Advance(size_t bytes)
    {
        BeginPtr += bytes;
    }

    bool IsFinished() const
    {
        return true;
    }

    void SetBuffer(const char* begin, const char* end)
    {
        BeginPtr = begin;
        EndPtr = end;
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TParserCoroutine>
class TBlockReader
{
private:
    TParserCoroutine& Coroutine;

    const char* BeginPtr;
    const char* EndPtr;
    bool FinishFlag;

public:
    TBlockReader(
        TParserCoroutine& coroutine,
        const char* begin,
        const char* end,
        bool finish)
        : Coroutine(coroutine)
        , BeginPtr(begin)
        , EndPtr(end)
        , FinishFlag(finish)
    { }

    const char* Begin() const
    {
        return BeginPtr;
    }

    const char* End() const
    {
        return EndPtr;
    }

    void RefreshBlock()
    {
        std::tie(BeginPtr, EndPtr, FinishFlag) = Coroutine.Yield(0);
    }

    void Advance(size_t bytes)
    {
        BeginPtr += bytes;
    }

    bool IsFinished() const
    {
        return FinishFlag;
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
