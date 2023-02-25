#ifndef TOKEN_INL_H_
#error "Direct inclusion of this file is not allowed, include token.h"
// For the sake of sane code completion.
#include "token.h"
#endif

namespace NYT::NYPath {

////////////////////////////////////////////////////////////////////////////////

template <class E>
requires TEnumTraits<E>::IsEnum
TString ToYPathLiteral(E value)
{
    return FormatEnum(value);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYPath

