#include "registry.h"

#include <yt/core/misc/common.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TToolRegistry* GetToolRegistry()
{
    return Singleton<TToolRegistry>();
}

////////////////////////////////////////////////////////////////////////////////

TToolRegistryEntry::TToolRegistryEntry(Stroka&& typeName, Stroka&& toolName, TGenericTool tool)
{
    auto* registry = GetToolRegistry();
    YCHECK(registry != nullptr);

    bool added = registry->emplace(std::move(typeName), TToolDescription{std::move(toolName), tool}).second;
    YCHECK(added);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
