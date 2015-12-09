#include "tclap_helpers.h"

namespace std {

////////////////////////////////////////////////////////////////////////////////

Stroka ReadAll(std::istringstream& input)
{
    Stroka result(input.str());
    input.ignore(std::numeric_limits<std::streamsize>::max());
    return result;
}

std::istringstream& operator >> (std::istringstream& input, NYT::TGuid& guid)
{
    auto str = ReadAll(input);
    guid = NYT::TGuid::FromString(str);
    return input;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace std

