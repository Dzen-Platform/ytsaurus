#pragma once

#include "public.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

// Forward declaration, avoid including format.h directly here.
template <class... TArgs>
void Format(TStringBuilder* builder, const char* format, const TArgs&... args);

//! A simple helper for constructing strings by a sequence of appends.
class TStringBuilder
    : private TNonCopyable
{
public:
    char* Preallocate(size_t size)
    {
        // Fast path.
        if (Current_ + size <= End_) {
            return Current_;
        }

        // Slow path.
        size_t committedLength = GetLength();
        size = std::max(size, MinBufferLength);
        Str_.ReserveAndResize(committedLength + size);
        Begin_ = Current_ = &*Str_.begin() + committedLength;
        End_ = Begin_ + size;
        return Current_;
    }

    size_t GetLength() const
    {
        return Current_ ? Current_ - Str_.Data() : 0;
    }

    TStringBuf GetBuffer() const
    {
        return TStringBuf(&*Str_.begin(), GetLength());
    }

    void Advance(size_t size)
    {
        Current_ += size;
        Y_ASSERT(Current_ <= End_);
    }

    void AppendChar(char ch)
    {
        *Preallocate(1) = ch;
        Advance(1);
    }

    void AppendChar(char ch, int n)
    {
        Y_ASSERT(n >= 0);
        char* dst = Preallocate(n);
        memset(dst, ch, n);
        Advance(n);
    }

    void AppendString(const TStringBuf& str)
    {
        char* dst = Preallocate(str.length());
        memcpy(dst, str.begin(), str.length());
        Advance(str.length());
    }

    void AppendString(const char* str)
    {
        AppendString(TStringBuf(str));
    }

    template <class... TArgs>
    void AppendFormat(const char* format, const TArgs&... args)
    {
        Format(this, format, args...);
    }

    Stroka Flush()
    {
        Str_.resize(GetLength());
        Begin_ = Current_ = End_ = nullptr;
        return std::move(Str_);
    }

private:
    Stroka Str_;

    char* Begin_ = nullptr;
    char* Current_ = nullptr;
    char* End_ = nullptr;

    static const size_t MinBufferLength;

};

inline void FormatValue(TStringBuilder* builder, const TStringBuilder& value, const TStringBuf& /*format*/)
{
    builder->AppendString(value.GetBuffer());
}

template <class T>
Stroka ToStringViaBuilder(const T& value, const TStringBuf& spec = STRINGBUF("v"))
{
    TStringBuilder builder;
    FormatValue(&builder, value, spec);
    return builder.Flush();
}

////////////////////////////////////////////////////////////////////////////////

//! Formatters enable customizable way to turn an object into a string.
//! This default implementation uses |FormatValue|.
struct TDefaultFormatter
{
    template <class T>
    void operator()(TStringBuilder* builder, const T& obj) const
    {
        FormatValue(builder, obj, STRINGBUF("v"));
    }
};

extern const TStringBuf DefaultJoinToStringDelimiter;

//! Joins a range of items into a string intermixing them with the delimiter.
/*!
 *  \param builder String builder where the output goes.
 *  \param begin Iterator pointing to the first item (inclusive).
 *  \param end Iterator pointing to the last item (not inclusive).
 *  \param formatter Formatter to apply to the items.
 *  \param delimiter A delimiter to be inserted between items: ", " by default.
 *  \return The resulting combined string.
 */
template <class TIterator, class TFormatter>
void JoinToString(
    TStringBuilder* builder,
    const TIterator& begin,
    const TIterator& end,
    const TFormatter& formatter,
    const TStringBuf& delimiter = DefaultJoinToStringDelimiter)
{
    for (auto current = begin; current != end; ++current) {
        if (current != begin) {
            builder->AppendString(delimiter);
        }
        formatter(builder, *current);
    }
}

template <class TIterator, class TFormatter>
Stroka JoinToString(
    const TIterator& begin,
    const TIterator& end,
    const TFormatter& formatter,
    const TStringBuf& delimiter = DefaultJoinToStringDelimiter)
{
    TStringBuilder builder;
    JoinToString(&builder, begin, end, formatter, delimiter);
    return builder.Flush();
}

//! A handy shortcut with default formatter.
template <class TIterator>
Stroka JoinToString(
    const TIterator& begin,
    const TIterator& end,
    const TStringBuf& delimiter = DefaultJoinToStringDelimiter)
{
    return JoinToString(begin, end, TDefaultFormatter(), delimiter);
}

//! Joins a collection of given items into a string intermixing them with the delimiter.
/*!
 *  \param collection A collection containing the items to be joined.
 *  \param formatter Formatter to apply to the items.
 *  \param delimiter A delimiter to be inserted between items; ", " by default.
 */
template <class TCollection, class TFormatter>
Stroka JoinToString(
    const TCollection& collection,
    const TFormatter& formatter,
    const TStringBuf& delimiter = DefaultJoinToStringDelimiter)
{
    using std::begin;
    using std::end;
    return JoinToString(begin(collection), end(collection), formatter, delimiter);
}

//! A handy shortcut with the default formatter.
template <class TCollection>
Stroka JoinToString(
    const TCollection& collection,
    const TStringBuf& delimiter = DefaultJoinToStringDelimiter)
{
    return JoinToString(collection, TDefaultFormatter(), delimiter);
}

//! Converts a range of items into strings.
template <class TIter, class TFormatter>
std::vector<Stroka> ConvertToStrings(
    const TIter& begin,
    const TIter& end,
    const TFormatter& formatter,
    size_t maxSize = std::numeric_limits<size_t>::max())
{
    std::vector<Stroka> result;
    for (auto it = begin; it != end; ++it) {
        TStringBuilder builder;
        formatter(&builder, *it);
        result.push_back(builder.Flush());
        if (result.size() == maxSize) {
            break;
        }
    }
    return result;
}

//! A handy shortcut with the default formatter.
template <class TIter>
std::vector<Stroka> ConvertToStrings(
    const TIter& begin,
    const TIter& end,
    size_t maxSize = std::numeric_limits<size_t>::max())
{
    return ConvertToStrings(begin, end, TDefaultFormatter(), maxSize);
}

//! Converts a given collection of items into strings.
/*!
 *  \param collection A collection containing the items to be converted.
 *  \param formatter Formatter to apply to the items.
 *  \param maxSize Size limit for the resulting vector.
 */
template <class TCollection, class TFormatter>
std::vector<Stroka> ConvertToStrings(
    const TCollection& collection,
    const TFormatter& formatter,
    size_t maxSize = std::numeric_limits<size_t>::max())
{
    using std::begin;
    using std::end;
    return ConvertToStrings(begin(collection), end(collection), formatter, maxSize);
}

//! A handy shortcut with default formatter.
template <class TCollection>
std::vector<Stroka> ConvertToStrings(
    const TCollection& collection,
    size_t maxSize = std::numeric_limits<size_t>::max())
{
    return ConvertToStrings(collection, TDefaultFormatter(), maxSize);
}

////////////////////////////////////////////////////////////////////////////////

void UnderscoreCaseToCamelCase(TStringBuilder* builder, const TStringBuf& str);
Stroka UnderscoreCaseToCamelCase(const TStringBuf& str);

void CamelCaseToUnderscoreCase(TStringBuilder* builder, const TStringBuf& str);
Stroka CamelCaseToUnderscoreCase(const TStringBuf& str);

Stroka TrimLeadingWhitespaces(const Stroka& str);
Stroka Trim(const Stroka& str, const Stroka& whitespaces);

bool ParseBool(const Stroka& value);
TStringBuf FormatBool(bool value);

////////////////////////////////////////////////////////////////////////////////

//! Implemented for |[u]i(32|64)|.
template <class T>
char* WriteIntToBufferBackwards(char* ptr, T value);

char* WriteGuidToBuffer(char* ptr, const TGuid& value);

////////////////////////////////////////////////////////////////////////////////

Stroka DecodeEnumValue(const Stroka& value);
Stroka EncodeEnumValue(const Stroka& value);

template <class T>
T ParseEnum(const Stroka& value, typename TEnumTraits<T>::TType* = 0)
{
    return TEnumTraits<T>::FromString(DecodeEnumValue(value));
}

template <class T>
Stroka FormatEnum(T value, typename TEnumTraits<T>::TType* = 0)
{
    return EncodeEnumValue(ToString(value));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
