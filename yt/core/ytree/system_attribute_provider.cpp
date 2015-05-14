#include "stdafx.h"
#include "system_attribute_provider.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

TNullable<ISystemAttributeProvider::TAttributeInfo> ISystemAttributeProvider::FindBuiltinAttributeInfo(const Stroka& key)
{
    std::vector<TAttributeInfo> builtinAttributes;
    ListBuiltinAttributes(&builtinAttributes);
    auto it = std::find_if(
        builtinAttributes.begin(),
        builtinAttributes.end(),
        [&] (const ISystemAttributeProvider::TAttributeInfo& info) {
            return info.Key == key;
        });
    return it == builtinAttributes.end() ? Null : MakeNullable(*it);
}

void ISystemAttributeProvider::ListBuiltinAttributes(std::vector<TAttributeInfo>* attributes)
{
    std::vector<TAttributeInfo> systemAttributes;
    ListSystemAttributes(&systemAttributes);

    for (const auto& attribute : systemAttributes) {
        if (!attribute.IsCustom) {
            (*attributes).push_back(attribute);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
