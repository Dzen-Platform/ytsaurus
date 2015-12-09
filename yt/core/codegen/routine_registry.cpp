#include "routine_registry.h"
#include "private.h"

namespace NYT {
namespace NCodegen {

////////////////////////////////////////////////////////////////////////////////

// MangleSymbol adds underscore for Darwin platform.
Stroka MangleSymbol(const Stroka& name)
{
#ifdef _darwin_
    return "_" + name;
#else
    return name;
#endif
}

// DemangleSymbol strips the prefixed underscore on Darwin,
// returns empty string in case of non-prefixed name.
Stroka DemangleSymbol(const Stroka& name)
{
#ifdef _darwin_
    if (name.empty() || name[0] != '_') {
        return Stroka();
    } else {
        return name.substr(1);
    }
#else
    return name;
#endif
}

////////////////////////////////////////////////////////////////////////////////

uint64_t TRoutineRegistry::GetAddress(const Stroka& symbol) const
{
    auto it = SymbolToAddress_.find(symbol);
    YCHECK(it != SymbolToAddress_.end());
    return it->second;
}

TRoutineRegistry::TTypeBuilder TRoutineRegistry::GetTypeBuilder(const Stroka& symbol) const
{
    auto mangledSymbol = MangleSymbol(symbol);
    auto it = SymbolToTypeBuilder_.find(mangledSymbol);
    YCHECK(it != SymbolToTypeBuilder_.end());
    return it->second;
}

void TRoutineRegistry::RegisterRoutineImpl(
    const char* symbol,
    uint64_t address,
    TTypeBuilder typeBuilder)
{
    auto mangledSymbol = MangleSymbol(symbol);
    YCHECK(SymbolToAddress_.insert(std::make_pair(mangledSymbol, std::move(address))).second);
    YCHECK(SymbolToTypeBuilder_.insert(std::make_pair(mangledSymbol, std::move(typeBuilder))).second);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCodegen
} // namespace NYT

