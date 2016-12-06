#include "function_context.h"

#include <yt/core/misc/assert.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TFunctionContext::TImpl
{
public:
    TImpl(std::vector<bool> literalArgs)
        : LiteralArgs_(std::move(literalArgs))
    { }

    void* CreateUntypedObject(void* pointer, void(*deleter)(void*))
    {
        try {
            auto holder = std::unique_ptr<void, void(*)(void*)>(pointer, deleter);
            Objects_.push_back(std::move(holder));
            return pointer;
        } catch (...) {
            return nullptr;
        }
    }

    void* GetPrivateData() const
    {
        return PrivateData_;
    }

    void SetPrivateData(void* privateData)
    {
        PrivateData_ = privateData;
    }

    bool IsLiteralArg(int argIndex) const
    {
        Y_ASSERT(argIndex >= 0 && argIndex < LiteralArgs_.size());
        return LiteralArgs_[argIndex];
    }

private:
    std::vector<bool> LiteralArgs_;

    std::vector<std::unique_ptr<void, void(*)(void*)>> Objects_;

    void* PrivateData_ = nullptr;
};

TFunctionContext::TFunctionContext(std::vector<bool> literalArgs)
    : Impl_(std::make_unique<TImpl>(std::move(literalArgs)))
{ }

TFunctionContext::~TFunctionContext() = default;

void* TFunctionContext::CreateUntypedObject(void* pointer, void(*deleter)(void*))
{
    return Impl_->CreateUntypedObject(pointer, deleter);
}

void* TFunctionContext::GetPrivateData() const
{
    return Impl_->GetPrivateData();
}

void TFunctionContext::SetPrivateData(void* data)
{
    Impl_->SetPrivateData(data);
}

bool TFunctionContext::IsLiteralArg(int argIndex) const
{
    return Impl_->IsLiteralArg(argIndex);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

