#include "serialized_channel.h"
#include "channel_detail.h"
#include "client.h"

#include <queue>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

class TSerializedChannel;
typedef TIntrusivePtr<TSerializedChannel> TSerializedChannelPtr;

class TSerializedChannel
    : public TChannelWrapper
{
public:
    explicit TSerializedChannel(IChannelPtr underlyingChannel)
        : TChannelWrapper(std::move(underlyingChannel))
    { }

    virtual IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        const TSendOptions& options) override
    {
        auto entry = New<TEntry>(
            request,
            responseHandler,
            options);

        {
            TGuard<TSpinLock> guard(SpinLock_);
            Queue_.push(entry);
        }

        TrySendQueuedRequests();

        return entry->RequestControlThunk;
    }

    virtual TFuture<void> Terminate(const TError& /*error*/) override
    {
        Y_UNREACHABLE();
    }

    void OnRequestCompleted()
    {
        {
            TGuard<TSpinLock> guard(SpinLock_);
            YCHECK(RequestInProgress_);
            RequestInProgress_ = false;
        }

        TrySendQueuedRequests();
    }

private:
    class TResponseHandler
        : public IClientResponseHandler
    {
    public:
        TResponseHandler(
            IClientResponseHandlerPtr underlyingHandler,
            TSerializedChannelPtr owner)
            : UnderlyingHandler_(std::move(underlyingHandler))
            , Owner_(std::move(owner))
        { }

        virtual void HandleAcknowledgement() override
        {
            UnderlyingHandler_->HandleAcknowledgement();
        }

        virtual void HandleResponse(TSharedRefArray message) override
        {
            UnderlyingHandler_->HandleResponse(std::move(message));
            Owner_->OnRequestCompleted();
        }

        virtual void HandleError(const TError& error) override
        {
            UnderlyingHandler_->HandleError(error);
            Owner_->OnRequestCompleted();
        }

        virtual void HandleStreamingPayload(const TStreamingPayload& payload) override
        {
            UnderlyingHandler_->HandleStreamingPayload(payload);
        }

        virtual void HandleStreamingFeedback(const TStreamingFeedback& feedback) override
        {
            UnderlyingHandler_->HandleStreamingFeedback(feedback);
        }

    private:
        const IClientResponseHandlerPtr UnderlyingHandler_;
        const TSerializedChannelPtr Owner_;

    };

    struct TEntry
        : public TIntrinsicRefCounted
    {
        TEntry(
            IClientRequestPtr request,
            IClientResponseHandlerPtr handler,
            const TSendOptions& options)
            : Request(std::move(request))
            , Handler(std::move(handler))
            , Options(options)
        { }

        IClientRequestPtr Request;
        IClientResponseHandlerPtr Handler;
        TSendOptions Options;
        TClientRequestControlThunkPtr RequestControlThunk = New<TClientRequestControlThunk>();
    };

    typedef TIntrusivePtr<TEntry> TEntryPtr;

    TSpinLock SpinLock_;
    std::queue<TEntryPtr> Queue_;
    bool RequestInProgress_ = false;


    void TrySendQueuedRequests()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        while (!RequestInProgress_ && !Queue_.empty()) {
            auto entry = Queue_.front();
            Queue_.pop();
            RequestInProgress_ = true;
            guard.Release();

            auto serializedHandler = New<TResponseHandler>(entry->Handler, this);
            auto requestControl = UnderlyingChannel_->Send(
                entry->Request,
                serializedHandler,
                entry->Options);
            entry->RequestControlThunk->SetUnderlying(std::move(requestControl));
            entry->Request.Reset();
            entry->Handler.Reset();
            entry->RequestControlThunk.Reset();
        }
    }

};

IChannelPtr CreateSerializedChannel(IChannelPtr underlyingChannel)
{
    YCHECK(underlyingChannel);

    return New<TSerializedChannel>(std::move(underlyingChannel));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
