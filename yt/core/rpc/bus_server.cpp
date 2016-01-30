#include "bus_server.h"
#include "private.h"
#include "server_detail.h"

#include <yt/core/bus/bus.h>
#include <yt/core/bus/server.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/rpc/message.h>
#include <yt/core/rpc/rpc.pb.h>

namespace NYT {
namespace NRpc {

using namespace NConcurrency;
using namespace NBus;

using ::ToString;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = RpcServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TBusServer
    : public TServerBase
    , public IMessageHandler
{
public:
    explicit TBusServer(IBusServerPtr busServer)
        : BusServer_(std::move(busServer))
    { }

private:
    IBusServerPtr BusServer_;


    virtual void DoStart() override
    {
        TServerBase::DoStart();

        BusServer_->Start(this);
    }

    virtual void DoStop() override
    {
        TServerBase::DoStop();

        BusServer_->Stop();
        BusServer_.Reset();
    }

    virtual void HandleMessage(TSharedRefArray message, IBusPtr replyBus) override
    {
        auto messageType = GetMessageType(message);
        switch (messageType) {
            case EMessageType::Request:
                OnRequestMessage(std::move(message), std::move(replyBus));
                break;

            case EMessageType::RequestCancelation:
                OnRequestCancelationMessage(std::move(message), std::move(replyBus));
                break;

            default:
                // Unable to reply, no request id is known.
                // Let's just drop the message.
                LOG_ERROR("Invalid incoming message type %x, ignored", static_cast<ui32>(messageType));
                break;
        }
    }

    void OnRequestMessage(TSharedRefArray message, IBusPtr replyBus)
    {
        auto header = std::make_unique<NProto::TRequestHeader>();
        if (!ParseRequestHeader(message, header.get())) {
            // Unable to reply, no request id is known.
            // Let's just drop the message.
            LOG_ERROR("Error parsing request header");
            return;
        }

        auto requestId = FromProto<TRequestId>(header->request_id());
        const auto& serviceName = header->service();
        const auto& methodName = header->method();
        auto realmId = header->has_realm_id() ? FromProto<TRealmId>(header->realm_id()) : NullRealmId;
        bool isOneWay = header->one_way();
        auto timeout = header->has_timeout() ? MakeNullable(FromProto<TDuration>(header->timeout())) : Null;
        auto startTime = header->has_start_time() ? MakeNullable(FromProto<TInstant>(header->start_time())) : Null;
        bool isRetry = header->retry();

        if (message.Size() < 2) {
            LOG_ERROR("Too few request parts: expected >= 2, actual %v (RequestId: %v)",
                message.Size(),
                requestId);
            return;
        }

        LOG_DEBUG("Request received (Method: %v:%v, RealmId: %v, RequestId: %v, User: %v, OneWay: %v, "
            "Timeout: %v, Endpoint: %v, StartTime: %v, Retry: %v)",
            serviceName,
            methodName,
            realmId,
            requestId,
            header->has_user() ? header->user() : RootUserName,
            isOneWay,
            timeout,
            replyBus->GetEndpointDescription(),
            startTime,
            isRetry);

        if (!Started_) {
            auto error = TError(NRpc::EErrorCode::Unavailable, "Server is not started");

            LOG_DEBUG(error);

            if (!isOneWay) {
                auto response = CreateErrorResponseMessage(requestId, error);
                replyBus->Send(response, EDeliveryTrackingLevel::None);
            }
            return;
        }

        TServiceId serviceId(serviceName, realmId);
        auto service = FindService(serviceId);
        if (!service) {
            auto error = TError(
                EErrorCode::NoSuchService,
                "Service is not registered")
                 << TErrorAttribute("service", serviceName)
                 << TErrorAttribute("realm_id", realmId);

            LOG_WARNING(error);

            if (!isOneWay) {
                auto response = CreateErrorResponseMessage(requestId, error);
                replyBus->Send(std::move(response), EDeliveryTrackingLevel::None);
            }
            return;
        }

        service->HandleRequest(
            std::move(header),
            std::move(message),
            std::move(replyBus));
    }

    void OnRequestCancelationMessage(TSharedRefArray message, IBusPtr /*replyBus*/)
    {
        NProto::TRequestCancelationHeader header;
        if (!ParseRequestCancelationHeader(message, &header)) {
            // Unable to reply, no request id is known.
            // Let's just drop the message.
            LOG_ERROR("Error parsing request cancelation header");
            return;
        }

        auto requestId = FromProto<TRequestId>(header.request_id());
        const auto& serviceName = header.service();
        const auto& methodName = header.method();
        auto realmId = header.has_realm_id() ? FromProto<TRealmId>(header.realm_id()) : NullRealmId;

        TServiceId serviceId(serviceName, realmId);
        auto service = FindService(serviceId);
        if (!service) {
            LOG_DEBUG("Service is not registered (Service: %v, RealmId: %v, RequestId: %v)",
                serviceName,
                realmId,
                requestId);
            return;
        }

        LOG_DEBUG("Request cancelation received (Method: %v:%v, RealmId: %v, RequestId: %v)",
            serviceName,
            methodName,
            realmId,
            requestId);

        service->HandleRequestCancelation(requestId);
    }
};

IServerPtr CreateBusServer(NBus::IBusServerPtr busServer)
{
    return New<TBusServer>(busServer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
