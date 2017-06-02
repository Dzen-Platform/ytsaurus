#pragma once

#include "private.h"
#include "bus.h"
#include "packet.h"
#include "tcp_dispatcher_impl.h"

#include <yt/core/actions/future.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/lock_free.h>
#include <yt/core/misc/ring_queue.h>

#include <yt/core/concurrency/poller.h>
#include <yt/core/concurrency/rw_spinlock.h>

#include <util/network/init.h>

#include <atomic>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETcpConnectionState,
    (None)
    (Resolving)
    (Opening)
    (Open)
    (Closed)
    (Aborted)
);

class TTcpConnection
    : public IBus
    , public NConcurrency::IPollable
{
public:
    TTcpConnection(
        TTcpBusConfigPtr config,
        EConnectionType connectionType,
        TNullable<ETcpInterfaceType> interfaceType,
        const TConnectionId& id,
        int socket,
        const Stroka& endpointDescription,
        const NYTree::IAttributeDictionary& endpointAttributes,
        const TNullable<Stroka>& address,
        const TNullable<Stroka>& unixDomainName,
        int priority,
        IMessageHandlerPtr handler,
        NConcurrency::IPollerPtr poller);

    ~TTcpConnection();

    void Start();
    void Check();

    const TConnectionId& GetId() const;

    // IPollable implementation.
    virtual const Stroka& GetLoggingId() const override;
    virtual void OnEvent(NConcurrency::EPollControl control) override;
    virtual void OnShutdown() override;

    // IBus implementation.
    virtual const Stroka& GetEndpointDescription() const override;
    virtual const NYTree::IAttributeDictionary& GetEndpointAttributes() const override;
    virtual TFuture<void> Send(TSharedRefArray message, const TSendOptions& options) override;
    virtual void Terminate(const TError& error) override;

    DECLARE_SIGNAL(void(const TError&), Terminated);

private:
    using EState = ETcpConnectionState;

    struct TQueuedMessage
    {
        TQueuedMessage() = default;

        TQueuedMessage(TSharedRefArray message, const TSendOptions& options)
            : Promise(options.TrackingLevel != EDeliveryTrackingLevel::None ? NewPromise<void>() : Null)
            , Message(std::move(message))
            , Options(options)
            , PacketId(TPacketId::Create())
        { }

        TPromise<void> Promise;
        TSharedRefArray Message;
        TSendOptions Options;
        TPacketId PacketId;
    };

    struct TPacket
    {
        TPacket(
            EPacketType type,
            EPacketFlags flags,
            int checksummedPartCount,
            const TPacketId& packetId,
            TSharedRefArray message,
            size_t size)
            : Type(type)
            , Flags(flags)
            , ChecksummedPartCount(checksummedPartCount)
            , PacketId(packetId)
            , Message(std::move(message))
            , Size(size)
        { }

        EPacketType Type;
        EPacketFlags Flags;
        int ChecksummedPartCount;
        TPacketId PacketId;
        TSharedRefArray Message;
        size_t Size;
    };

    struct TUnackedMessage
    {
        TUnackedMessage() = default;

        TUnackedMessage(const TPacketId& packetId, TPromise<void> promise)
            : PacketId(packetId)
            , Promise(std::move(promise))
        { }

        TPacketId PacketId;
        TPromise<void> Promise;
    };

    const TTcpBusConfigPtr Config_;
    const EConnectionType ConnectionType_;
    const TConnectionId Id_;
    const Stroka EndpointDescription_;
    const std::unique_ptr<NYTree::IAttributeDictionary> EndpointAttributes_;
    const TNullable<Stroka> Address_;
    const TNullable<Stroka> UnixDomainName_;
#ifdef _linux_
    const int Priority_;
#endif
    const IMessageHandlerPtr Handler_;
    const NConcurrency::IPollerPtr Poller_;

    const NLogging::TLogger Logger;
    const Stroka LoggingId_;

    TNullable<ETcpInterfaceType> InterfaceType_;
    TTcpDispatcherCountersPtr Counters_;
    bool GenerateChecksums_ = true;
    bool ConnectionCounterIncremented_ = false;

    // Only used by client sockets.
    int Port_ = 0;

    std::atomic<EState> State_ = {EState::None};

    TSpinLock EventHandlerSpinLock_;
    NConcurrency::TReaderWriterSpinLock ControlSpinLock_;

    TError TerminateError_;
    bool TerminateRequested_ = false;
    int Socket_;

    bool Unregistered_ = false;
    TError CloseError_;

    TSingleShotCallbackList<void(const TError&)> Terminated_;

    std::atomic<bool> ArmedForQueuedMessages_ = {false};
    TMultipleProducerSingleConsumerLockFreeStack<TQueuedMessage> QueuedMessages_;

    TPacketDecoder Decoder_;
    NProfiling::TCpuDuration ReadStallTimeout_;
    std::atomic<NProfiling::TCpuInstant> LastIncompleteReadTime_ = {std::numeric_limits<NProfiling::TCpuInstant>::max()};
    TBlob ReadBuffer_;

    TRingQueue<TPacket*> QueuedPackets_;
    TRingQueue<TPacket*> EncodedPackets_;

    TPacketEncoder Encoder_;
    NProfiling::TCpuDuration WriteStallTimeout_;
    std::atomic<NProfiling::TCpuInstant> LastIncompleteWriteTime_ = {std::numeric_limits<NProfiling::TCpuInstant>::max()};
    std::vector<std::unique_ptr<TBlob>> WriteBuffers_;
    TRingQueue<TRef> EncodedFragments_;
    TRingQueue<size_t> EncodedPacketSizes_;

    std::vector<struct iovec> SendVector_;

    TRingQueue<TUnackedMessage> UnackedMessages_;

    void Cleanup();

    void Open();
    void ResolveAddress();
    void Abort(const TError& error);

    void InitBuffers();

    int GetSocketPort();

    void ConnectSocket(const TNetworkAddress& address);
    void CloseSocket();

    void OnAddressResolveFinished(const TErrorOr<TNetworkAddress>& result);
    void OnAddressResolved(const TNetworkAddress& address, ETcpInterfaceType interfaceType, NConcurrency::TWriterGuard& guard);
    void SetupInterfaceType(ETcpInterfaceType interfaceType);

    int GetSocketError() const;
    bool IsSocketError(ssize_t result);

    void OnSocketConnected();

    void OnSocketRead();
    bool HasUnreadData() const;
    bool ReadSocket(char* buffer, size_t size, size_t* bytesRead);
    bool CheckReadError(ssize_t result);
    bool AdvanceDecoder(size_t size);
    bool OnPacketReceived() throw();
    bool OnAckPacketReceived();
    bool OnMessagePacketReceived();

    TPacket* EnqueuePacket(
        EPacketType type,
        EPacketFlags flags,
        int checksummedPartCount,
        const TPacketId& packetId,
        TSharedRefArray message = TSharedRefArray());
    void OnSocketWrite();
    bool HasUnsentData() const;
    bool WriteFragments(size_t* bytesWritten);
    void FlushWrittenFragments(size_t bytesWritten);
    void FlushWrittenPackets(size_t bytesWritten);
    bool MaybeEncodeFragments();
    bool CheckWriteError(ssize_t result);
    void OnPacketSent();
    void OnAckPacketSent(const TPacket& packet);
    void OnMessagePacketSent(const TPacket& packet);
    void OnTerminated();
    void OnMessageEnqueued();
    void ProcessQueuedMessages();
    void DiscardOutcomingMessages(const TError& error);
    void DiscardUnackedMessages(const TError& error);

    void UnregisterFromPoller();

    void TryArmPoller();
    void DoArmPoller();
    void RearmPoller();

    void UpdateConnectionCount(bool increment);
    void UpdatePendingOut(int countDelta, i64 sizeDelta);

};

DEFINE_REFCOUNTED_TYPE(TTcpConnection)

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
