#pragma once

#include "public.h"
#include "mutation_context.h"

#include <core/misc/error.h>
#include <core/misc/ref.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class TMutation
    : public TIntrinsicRefCounted
{
public:
    explicit TMutation(IHydraManagerPtr hydraManager);

    TFuture<TMutationResponse> Commit();

    TMutationPtr SetRequestData(TSharedRef data, Stroka type);
    template <class TRequest>
    TMutationPtr SetRequestData(const TRequest& request);

    TMutationPtr SetAction(TCallback<void(TMutationContext*)> action);
    template <class TResponse>
    TMutationPtr SetAction(TCallback<TResponse()> action);

    TMutationPtr SetAllowLeaderForwarding(bool value);

private:
    const IHydraManagerPtr HydraManager_;

    TMutationRequest Request_;

};

DEFINE_REFCOUNTED_TYPE(TMutation)

////////////////////////////////////////////////////////////////////////////////

TMutationPtr CreateMutation(IHydraManagerPtr hydraManager);

template <class TRequest>
TMutationPtr CreateMutation(IHydraManagerPtr hydraManager, const TRequest& request);

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

#define MUTATION_INL_H_
#include "mutation-inl.h"
#undef MUTATION_INL_H_
