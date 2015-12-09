#pragma once

#include "public.h"

#include <yt/core/misc/property.h>

namespace NYT {
namespace NYson {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETokenType,
    (EndOfStream) // Empty or uninitialized token

    (String)
    (Int64)
    (Uint64)
    (Double)
    (Boolean)

    // Special values:
    // YSON
    (Semicolon) // ;
    (Equals) // =
    (Hash) // #
    (LeftBracket) // [
    (RightBracket) // ]
    (LeftBrace) // {
    (RightBrace) // }
    (LeftAngle) // <
    (RightAngle) // >
    // Table ranges
    (LeftParenthesis) // (
    (RightParenthesis) // )
    (Plus) // +
    (Colon) // :
    (Comma) // ,
);

////////////////////////////////////////////////////////////////////////////////

ETokenType CharToTokenType(char ch);        // returns ETokenType::EndOfStream for non-special chars
char TokenTypeToChar(ETokenType type);      // YUNREACHABLE for non-special types
Stroka TokenTypeToString(ETokenType type);  // YUNREACHABLE for non-special types

////////////////////////////////////////////////////////////////////////////////

class TToken
{
public:
    static const TToken EndOfStream;

    TToken();
    TToken(ETokenType type); // for special types
    explicit TToken(const TStringBuf& stringValue); // for string values
    explicit TToken(i64 int64Value); // for int64 values
    explicit TToken(ui64 int64Value); // for uint64 values
    explicit TToken(double doubleValue); // for double values
    explicit TToken(bool booleanValue); // for booleans

    DEFINE_BYVAL_RO_PROPERTY(ETokenType, Type);

    bool IsEmpty() const;
    const TStringBuf& GetStringValue() const;
    i64 GetInt64Value() const;
    ui64 GetUint64Value() const;
    double GetDoubleValue() const;
    bool GetBooleanValue() const;

    void ExpectType(ETokenType expectedType) const;
    void ExpectTypes(const std::vector<ETokenType>& expectedTypes) const;
    void ThrowUnexpected() const;

    void Reset();

private:
    TStringBuf StringValue_;
    i64 Int64Value_;
    ui64 Uint64Value_;
    double DoubleValue_;
    bool BooleanValue_;
};

Stroka ToString(const TToken& token);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson
} // namespace NYT
