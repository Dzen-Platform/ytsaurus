#pragma once

#include "server.h"
#include "service.h"
#include "authentication_identity.h"

#include <yt/core/ytalloc/memory_zone.h>

#include <yt/core/concurrency/spinlock.h>

#include <yt/core/logging/log.h>

#include <yt/core/rpc/proto/rpc.pb.h>

#include <atomic>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

//! \note Thread affinity: single-threaded (unless noted otherwise)
class TServiceContextBase
    : public IServiceContext
{
public:
    virtual const NProto::TRequestHeader& GetRequestHeader() const override;
    virtual TSharedRefArray GetRequestMessage() const override;

    virtual TRequestId GetRequestId() const override;
    virtual NYT::NBus::TTcpDispatcherStatistics GetBusStatistics() const override;
    virtual const NYTree::IAttributeDictionary& GetEndpointAttributes() const override;

    virtual std::optional<TInstant> GetStartTime() const override;
    virtual std::optional<TDuration> GetTimeout() const override;
    virtual bool IsRetry() const override;
    virtual TMutationId GetMutationId() const override;

    virtual const TString& GetService() const override;
    virtual const TString& GetMethod() const override;
    virtual TRealmId GetRealmId() const override;
    virtual const TAuthenticationIdentity& GetAuthenticationIdentity() const override;

    //! \note Thread affinity: any
    virtual bool IsReplied() const override;

    virtual void Reply(const TError& error) override;
    virtual void Reply(const TSharedRefArray& responseMessage) override;
    using IServiceContext::Reply;

    virtual void SetComplete() override;

    //! \note Thread affinity: any
    virtual TFuture<TSharedRefArray> GetAsyncResponseMessage() const override;

    virtual const TSharedRefArray& GetResponseMessage() const override;

    virtual void SubscribeCanceled(const TClosure& callback) override;
    virtual void UnsubscribeCanceled(const TClosure& callback) override;

    virtual bool IsCanceled() override;
    virtual void Cancel() override;

    virtual const TError& GetError() const override;

    virtual TSharedRef GetRequestBody() const override;

    virtual TSharedRef GetResponseBody() override;
    virtual void SetResponseBody(const TSharedRef& responseBody) override;

    virtual std::vector<TSharedRef>& RequestAttachments() override;
    virtual NConcurrency::IAsyncZeroCopyInputStreamPtr GetRequestAttachmentsStream() override;

    virtual std::vector<TSharedRef>& ResponseAttachments() override;
    virtual NConcurrency::IAsyncZeroCopyOutputStreamPtr GetResponseAttachmentsStream() override;

    virtual const NProto::TRequestHeader& RequestHeader() const override;
    virtual NProto::TRequestHeader& RequestHeader() override;

    virtual void SetRawRequestInfo(TString info, bool incremental) override;
    virtual void SetRawResponseInfo(TString info, bool incremental) override;

    virtual const NLogging::TLogger& GetLogger() const override;
    virtual NLogging::ELogLevel GetLogLevel() const override;

    virtual bool IsPooled() const override;

    virtual NCompression::ECodec GetResponseCodec() const override;
    virtual void SetResponseCodec(NCompression::ECodec codec) override;

protected:
    const std::unique_ptr<NProto::TRequestHeader> RequestHeader_;
    const TSharedRefArray RequestMessage_;
    const NLogging::TLogger Logger;
    const NLogging::ELogLevel LogLevel_;

    TRequestId RequestId_;
    TRealmId RealmId_;

    TAuthenticationIdentity AuthenticationIdentity_;

    TSharedRef RequestBody_;
    std::vector<TSharedRef> RequestAttachments_;

    std::atomic<bool> Replied_ = false;
    TError Error_;

    TSharedRef ResponseBody_;
    std::vector<TSharedRef> ResponseAttachments_;

    SmallVector<TString, 4> RequestInfos_;
    SmallVector<TString, 4> ResponseInfos_;

    NCompression::ECodec ResponseCodec_ = NCompression::ECodec::None;

    TServiceContextBase(
        std::unique_ptr<NProto::TRequestHeader> header,
        TSharedRefArray requestMessage,
        NLogging::TLogger logger,
        NLogging::ELogLevel logLevel);
    TServiceContextBase(
        TSharedRefArray requestMessage,
        NLogging::TLogger logger,
        NLogging::ELogLevel logLevel);

    virtual void DoReply() = 0;
    virtual void DoFlush();

    virtual void LogRequest() = 0;
    virtual void LogResponse() = 0;

private:
    YT_DECLARE_SPINLOCK(TAdaptiveLock, ResponseLock_);
    TSharedRefArray ResponseMessage_; // cached
    mutable TPromise<TSharedRefArray> AsyncResponseMessage_; // created on-demand


    void Initialize();
    TSharedRefArray BuildResponseMessage();
    void ReplyEpilogue();
};

////////////////////////////////////////////////////////////////////////////////

class TServiceContextWrapper
    : public IServiceContext
{
public:
    explicit TServiceContextWrapper(IServiceContextPtr underlyingContext);

    virtual const NProto::TRequestHeader& GetRequestHeader() const override;
    virtual TSharedRefArray GetRequestMessage() const override;

    virtual NRpc::TRequestId GetRequestId() const override;
    virtual NYT::NBus::TTcpDispatcherStatistics GetBusStatistics() const override;
    virtual const NYTree::IAttributeDictionary& GetEndpointAttributes() const override;

    virtual std::optional<TInstant> GetStartTime() const override;
    virtual std::optional<TDuration> GetTimeout() const override;
    virtual bool IsRetry() const override;
    virtual TMutationId GetMutationId() const override;

    virtual const TString& GetService() const override;
    virtual const TString& GetMethod() const override;
    virtual TRealmId GetRealmId() const override;
    virtual const TAuthenticationIdentity& GetAuthenticationIdentity() const override;

    virtual bool IsReplied() const override;
    virtual void Reply(const TError& error) override;
    virtual void Reply(const TSharedRefArray& responseMessage) override;

    virtual void SetComplete() override;

    virtual TFuture<TSharedRefArray> GetAsyncResponseMessage() const override;
    virtual const TSharedRefArray& GetResponseMessage() const override;

    virtual void SubscribeCanceled(const TClosure& callback) override;
    virtual void UnsubscribeCanceled(const TClosure& callback) override;

    virtual bool IsCanceled() override;
    virtual void Cancel() override;

    virtual const TError& GetError() const override;

    virtual TSharedRef GetRequestBody() const override;

    virtual TSharedRef GetResponseBody() override;
    virtual void SetResponseBody(const TSharedRef& responseBody) override;

    virtual std::vector<TSharedRef>& RequestAttachments() override;
    virtual NConcurrency::IAsyncZeroCopyInputStreamPtr GetRequestAttachmentsStream() override;

    virtual std::vector<TSharedRef>& ResponseAttachments() override;
    virtual NConcurrency::IAsyncZeroCopyOutputStreamPtr GetResponseAttachmentsStream() override;

    virtual const NProto::TRequestHeader& RequestHeader() const override;
    virtual NProto::TRequestHeader& RequestHeader() override;

    virtual void SetRawRequestInfo(TString info, bool incremental) override;
    virtual void SetRawResponseInfo(TString info, bool incremental) override;

    virtual const NLogging::TLogger& GetLogger() const override;
    virtual NLogging::ELogLevel GetLogLevel() const override;

    virtual bool IsPooled() const override;

    virtual NCompression::ECodec GetResponseCodec() const override;
    virtual void SetResponseCodec(NCompression::ECodec codec) override;

protected:
    const IServiceContextPtr UnderlyingContext_;

};

////////////////////////////////////////////////////////////////////////////////

class TServerBase
    : public IServer
{
public:
    virtual void RegisterService(IServicePtr service) override;
    virtual bool UnregisterService(IServicePtr service) override;

    virtual IServicePtr FindService(const TServiceId& serviceId) override;

    virtual void Configure(TServerConfigPtr config) override;

    virtual void Start() override;
    virtual TFuture<void> Stop(bool graceful) override;

protected:
    const NLogging::TLogger Logger;

    std::atomic<bool> Started_ = false;

    YT_DECLARE_SPINLOCK(NConcurrency::TReaderWriterSpinLock, ServicesLock_);
    TServerConfigPtr Config_;
    THashMap<TServiceId, IServicePtr> ServiceMap_;

    explicit TServerBase(const NLogging::TLogger& logger);

    virtual void DoStart();
    virtual TFuture<void> DoStop(bool graceful);

    virtual void DoRegisterService(const IServicePtr& service);
    virtual void DoUnregisterService(const IServicePtr& service);

    std::vector<IServicePtr> DoFindServices(const TString& serviceName);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
