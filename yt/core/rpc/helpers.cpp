#include "stdafx.h"
#include "helpers.h"
#include "channel_detail.h"
#include "service.h"

#include <core/ytree/attribute_helpers.h>

namespace NYT {
namespace NRpc {

using namespace NRpc::NProto;
using namespace NTracing;

////////////////////////////////////////////////////////////////////////////////

void SetAuthenticatedUser(TRequestHeader* header, const Stroka& user)
{
    auto* ext = header->MutableExtension(TAuthenticatedExt::authenticated_ext);
    ext->set_user(user);
}

void SetAuthenticatedUser(IClientRequestPtr request, const Stroka& user)
{
    SetAuthenticatedUser(&request->Header(), user);
}

TNullable<Stroka> FindAuthenticatedUser(const TRequestHeader& header)
{
    return header.HasExtension(TAuthenticatedExt::authenticated_ext)
        ? TNullable<Stroka>(header.GetExtension(TAuthenticatedExt::authenticated_ext).user())
        : Null;
}

TNullable<Stroka> FindAuthenticatedUser(IServiceContextPtr context)
{
    return FindAuthenticatedUser(context->RequestHeader());
}

Stroka GetAuthenticatedUserOrThrow(IServiceContextPtr context)
{
    auto user = FindAuthenticatedUser(context);
    if (!user) {
        THROW_ERROR_EXCEPTION("Must specify an authenticated user in request header");
    }
    return user.Get();
}

////////////////////////////////////////////////////////////////////////////////

class TAuthenticatedChannel
    : public TChannelWrapper
{
public:
    TAuthenticatedChannel(IChannelPtr underlyingChannel, const Stroka& user)
        : TChannelWrapper(std::move(underlyingChannel))
        , User_(user)
    { }

    virtual IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        TNullable<TDuration> timeout,
        bool requestAck) override
    {
        SetAuthenticatedUser(request, User_);
        return UnderlyingChannel_->Send(
            request,
            responseHandler,
            timeout,
            requestAck);
    }

private:
    Stroka User_;

};

IChannelPtr CreateAuthenticatedChannel(IChannelPtr underlyingChannel, const Stroka& user)
{
    YCHECK(underlyingChannel);

    return New<TAuthenticatedChannel>(underlyingChannel, user);
}

////////////////////////////////////////////////////////////////////////////////

class TAuthenticatedChannelFactory
    : public IChannelFactory
{
public:
    TAuthenticatedChannelFactory(
        IChannelFactoryPtr underlyingFactory,
        const Stroka& user)
        : UnderlyingFactory_(underlyingFactory)
        , User_(user)
    { }

    virtual IChannelPtr CreateChannel(const Stroka& address) override
    {
        auto underlyingChannel = UnderlyingFactory_->CreateChannel(address);
        return CreateAuthenticatedChannel(underlyingChannel, User_);
    }

private:
    IChannelFactoryPtr UnderlyingFactory_;
    Stroka User_;

};

IChannelFactoryPtr CreateAuthenticatedChannelFactory(
    IChannelFactoryPtr underlyingFactory,
    const Stroka& user)
{
    YCHECK(underlyingFactory);

    return New<TAuthenticatedChannelFactory>(underlyingFactory, user);
}

////////////////////////////////////////////////////////////////////////////////

class TRealmChannel
    : public TChannelWrapper
{
public:
    TRealmChannel(IChannelPtr underlyingChannel, const TRealmId& realmId)
        : TChannelWrapper(std::move(underlyingChannel))
        , RealmId_(realmId)
    { }

    virtual IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        TNullable<TDuration> timeout,
        bool requestAck) override
    {
        ToProto(request->Header().mutable_realm_id(), RealmId_);
        return UnderlyingChannel_->Send(
            request,
            responseHandler,
            timeout,
            requestAck);
    }

private:
    TRealmId RealmId_;

};

IChannelPtr CreateRealmChannel(IChannelPtr underlyingChannel, const TRealmId& realmId)
{
    YCHECK(underlyingChannel);

    return New<TRealmChannel>(underlyingChannel, realmId);
}

////////////////////////////////////////////////////////////////////////////////

class TRealmChannelFactory
    : public IChannelFactory
{
public:
    TRealmChannelFactory(
        IChannelFactoryPtr underlyingFactory,
        const TRealmId& realmId)
        : UnderlyingFactory_(underlyingFactory)
        , RealmId_(realmId)
    { }

    virtual IChannelPtr CreateChannel(const Stroka& address) override
    {
        auto underlyingChannel = UnderlyingFactory_->CreateChannel(address);
        return CreateRealmChannel(underlyingChannel, RealmId_);
    }

private:
    IChannelFactoryPtr UnderlyingFactory_;
    TRealmId RealmId_;

};

IChannelFactoryPtr CreateRealmChannelFactory(
    IChannelFactoryPtr underlyingFactory,
    const TRealmId& realmId)
{
    YCHECK(underlyingFactory);

    return New<TRealmChannelFactory>(underlyingFactory, realmId);
}

////////////////////////////////////////////////////////////////////////////////

TTraceContext GetTraceContext(const TRequestHeader& header)
{
    if (!header.HasExtension(TTracingExt::tracing_ext)) {
        return TTraceContext();
    }

    const auto& ext = header.GetExtension(TTracingExt::tracing_ext);
    return TTraceContext(
        ext.trace_id(),
        ext.span_id(),
        ext.parent_span_id());
}

void SetTraceContext(TRequestHeader* header, const NTracing::TTraceContext& context)
{
    auto* ext = header->MutableExtension(TTracingExt::tracing_ext);
    ext->set_trace_id(context.GetTraceId());
    ext->set_span_id(context.GetSpanId());
    ext->set_parent_span_id(context.GetParentSpanId());
}

////////////////////////////////////////////////////////////////////////////////

TMutationId GenerateMutationId()
{
    return TMutationId::Create();
}

TMutationId GetMutationId(const TRequestHeader& header)
{
    if (!header.HasExtension(TMutatingExt::mutating_ext)) {
        return NullMutationId;
    }
    const auto& ext = header.GetExtension(TMutatingExt::mutating_ext);
    return FromProto<TMutationId>(ext.mutation_id());
}

TMutationId GetMutationId(IServiceContextPtr context)
{
    return GetMutationId(context->RequestHeader());
}

void GenerateMutationId(IClientRequestPtr request)
{
    SetMutationId(request, GenerateMutationId(), false);
}

void SetMutationId(TRequestHeader* header, const TMutationId& id, bool retry)
{
    auto* ext = header->MutableExtension(TMutatingExt::mutating_ext);
    if (id != NullMutationId) {
        ToProto(ext->mutable_mutation_id(), id);
        if (retry) {
            header->set_retry(true);
        }
    }
}

void SetMutationId(IClientRequestPtr request, const TMutationId& id, bool retry)
{
    SetMutationId(&request->Header(), id, retry);
}

void SetOrGenerateMutationId(IClientRequestPtr request, const TMutationId& id, bool retry)
{
    SetMutationId(request, id == NullMutationId ? TMutationId::Create() : id, retry);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
