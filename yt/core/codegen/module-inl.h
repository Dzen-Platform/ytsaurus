#pragma once
#ifndef CODEGEN_MODULE_INL_H_
#error "Direct inclusion of this file is not allowed, include skip_list.h"
#endif
#undef CODEGEN_MODULE_INL_H_

#include <yt/core/actions/bind.h>

namespace NYT {
namespace NCodegen {

////////////////////////////////////////////////////////////////////////////////

template <class TSignature>
TCGFunction<TSignature> TCGModule::GetCompiledFunction(const Stroka& name)
{
    auto type = llvm::TypeBuilder<TSignature, false>::get(GetContext());
    YCHECK(type == GetModule()->getFunction(name.c_str())->getFunctionType());
    return TCGFunction<TSignature>(GetFunctionAddress(name), MakeStrong(this));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCodegen
} // namespace NYT

