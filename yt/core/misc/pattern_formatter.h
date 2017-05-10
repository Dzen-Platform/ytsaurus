#pragma once

#include "common.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TPatternFormatter
    : private TNonCopyable
{
public:
    void AddProperty(const Stroka& name, const Stroka& value);
    Stroka Format(const Stroka& pattern);

private:
    typedef yhash<Stroka, Stroka> TPropertyMap;
    TPropertyMap PropertyMap;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
