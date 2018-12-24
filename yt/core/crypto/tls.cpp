#include "tls.h"

#include <yt/core/misc/error.h>
#include <yt/core/misc/ref.h>
#include <yt/core/misc/finally.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/poller.h>

#include <yt/core/net/connection.h>
#include <yt/core/net/dialer.h>
#include <yt/core/net/listener.h>

#include <yt/core/logging/log.h>

#include <contrib/libs/openssl/include/openssl/bio.h>
#include <contrib/libs/openssl/include/openssl/ssl.h>
#include <contrib/libs/openssl/include/openssl/err.h>
#include <contrib/libs/openssl/include/openssl/evp.h>
#include <contrib/libs/openssl/include/openssl/pem.h>

#include <library/openssl/io/stream.h>

namespace NYT::NCrypto {

using namespace NNet;
using namespace NConcurrency;
using namespace NLogging;

static const TLogger Logger{"Tls"};

////////////////////////////////////////////////////////////////////////////////

namespace {

TErrorAttribute GetLastSslError()
{
    char errorStr[256];
    ERR_error_string_n(ERR_get_error(), errorStr, sizeof(errorStr));
    return TErrorAttribute("ssl_error", TString(errorStr));
}

constexpr auto TlsBufferSize = 1_MB;

} // namespace

////////////////////////////////////////////////////////////////////////////////

struct TSslContextImpl
    : public TRefCounted
{
    SSL_CTX* Ctx = nullptr;

    TSslContextImpl()
    {
        Ctx = SSL_CTX_new(TLSv1_2_method());
        if (!Ctx) {
            THROW_ERROR_EXCEPTION("SSL_CTX_new(TLSv1_2_method()) failed")
                << GetLastSslError();
        }
    }

    ~TSslContextImpl()
    {
        if (Ctx) {
            SSL_CTX_free(Ctx);
        }
    }
};

DEFINE_REFCOUNTED_TYPE(TSslContextImpl)

////////////////////////////////////////////////////////////////////////////////

struct TTlsBufferTag
{ };

class TTlsConnection
    : public IConnection
{
public:
    TTlsConnection(
        TSslContextImplPtr ctx,
        IPollerPtr poller,
        IConnectionPtr connection)
        : Ctx_(std::move(ctx))
        , Invoker_(CreateSerializedInvoker(poller->GetInvoker()))
        , Underlying_(std::move(connection))
    {
        Ssl_ = SSL_new(Ctx_->Ctx);
        if (!Ssl_) {
            THROW_ERROR_EXCEPTION("SSL_new failed")
                << GetLastSslError();
        }

        InputBIO_ = BIO_new(BIO_s_mem());
        YCHECK(InputBIO_);
        // Makes InputBIO_ non-blocking.

        BIO_set_mem_eof_return(InputBIO_, -1);
        OutputBIO_ = BIO_new(BIO_s_mem());
        YCHECK(OutputBIO_);

        SSL_set_bio(Ssl_, InputBIO_, OutputBIO_);

        InputBuffer_ = TSharedMutableRef::Allocate<TTlsBufferTag>(TlsBufferSize);
        OutputBuffer_ = TSharedMutableRef::Allocate<TTlsBufferTag>(TlsBufferSize);
    }

    ~TTlsConnection()
    {
        SSL_free(Ssl_);
    }

    void StartClient()
    {
        SSL_set_connect_state(Ssl_);
        auto sslResult = SSL_do_handshake(Ssl_);
        sslResult = SSL_get_error(Ssl_, sslResult);
        YCHECK(sslResult == SSL_ERROR_WANT_READ);

        Invoker_->Invoke(BIND(&TTlsConnection::DoRun, MakeStrong(this)));
    }

    void StartServer()
    {
        SSL_set_accept_state(Ssl_);

        Invoker_->Invoke(BIND(&TTlsConnection::DoRun, MakeStrong(this)));
    }

    virtual int GetHandle() const override
    {
        Y_UNIMPLEMENTED();
    }

    virtual i64 GetReadByteCount() const override
    {
        return Underlying_->GetReadByteCount();
    }

    virtual TConnectionStatistics GetReadStatistics() const override
    {
        return Underlying_->GetReadStatistics();
    }

    virtual i64 GetWriteByteCount() const override
    {
        return Underlying_->GetWriteByteCount();
    }

    virtual const TNetworkAddress& LocalAddress() const override
    {
        return Underlying_->LocalAddress();
    }

    virtual const TNetworkAddress& RemoteAddress() const override
    {
        return Underlying_->RemoteAddress();
    }

    virtual TConnectionStatistics GetWriteStatistics() const override
    {
        return Underlying_->GetWriteStatistics();
    }

    virtual void SetReadDeadline(TInstant deadline) override
    {
        Underlying_->SetReadDeadline(deadline);
    }

    virtual void SetWriteDeadline(TInstant deadline) override
    {
        Underlying_->SetWriteDeadline(deadline);
    }

    virtual bool SetNoDelay() override
    {
        return Underlying_->SetNoDelay();
    }

    virtual bool SetKeepAlive() override
    {
        return Underlying_->SetKeepAlive();
    }

    virtual TFuture<size_t> Read(const TSharedMutableRef& buffer) override
    {
        auto promise = NewPromise<size_t>();
        ++ActiveIOCount_;
        Invoker_->Invoke(BIND([this, this_ = MakeStrong(this), promise, buffer] () {
            ReadBuffer_ = buffer;
            ReadPromise_ = promise;

            YCHECK(!ReadActive_);
            ReadActive_ = true;

            DoRun();
        }));
        return promise.ToFuture();
    }

    virtual TFuture<void> Write(const TSharedRef& buffer) override
    {
        return WriteV(TSharedRefArray(buffer));
    }

    virtual TFuture<void> WriteV(const TSharedRefArray& buffer) override
    {
        auto promise = NewPromise<void>();
        ++ActiveIOCount_;
        Invoker_->Invoke(BIND([this, this_ = MakeStrong(this), promise, buffer] () {
            WriteBuffer_ = buffer;
            WritePromise_ = promise;

            YCHECK(!WriteActive_);
            WriteActive_ = true;

            DoRun();
        }));
        return promise.ToFuture();
    }

    virtual TFuture<void> CloseRead() override
    {
        // TLS does not support half-open connection state.
        return Close();
    }

    virtual TFuture<void> CloseWrite() override
    {
        // TLS does not support half-open connection state.
        return Close();
    }

    virtual TFuture<void> Close() override
    {
        ++ActiveIOCount_;
        return BIND([this, this_ = MakeStrong(this)] () {
            CloseRequested_ = true;

            DoRun();
        })
            .AsyncVia(Invoker_)
            .Run();
    }

    virtual bool IsIdle() const override
    {
        return ActiveIOCount_ == 0 && !Failed_;
    }

    virtual TFuture<void> Abort() override
    {
        return BIND([this, this_ = MakeStrong(this)] () {
            if (Error_.IsOK()) {
                Error_ = TError("TLS connection aborted");
                CheckError();
            }
        })
            .AsyncVia(Invoker_)
            .Run();
    }

private:
    const TSslContextImplPtr Ctx_;
    const IInvokerPtr Invoker_;
    const IConnectionPtr Underlying_;

    SSL* Ssl_ = nullptr;
    BIO* InputBIO_ = nullptr;
    BIO* OutputBIO_ = nullptr;

    // This counter gets stuck after streams encounters an error.
    std::atomic<int> ActiveIOCount_ = {0};
    std::atomic<bool> Failed_ = {false};

    // FSM
    TError Error_;
    bool HandshakeInProgress_ = true;
    bool CloseRequested_ = false;
    bool ReadActive_ = false;
    bool WriteActive_ = false;
    bool UnderlyingReadActive_ = false;
    bool UnderlyingWriteActive_ = false;

    TSharedMutableRef InputBuffer_;
    TSharedMutableRef OutputBuffer_;

    // Active read
    TSharedMutableRef ReadBuffer_;
    TPromise<size_t> ReadPromise_;

    // Active write
    TSharedRefArray WriteBuffer_;
    TPromise<void> WritePromise_;


    void CheckError()
    {
        if (Error_.IsOK()) {
            return;
        }

        if (ReadActive_) {
            Failed_ = true;
            ReadPromise_.Set(Error_);
            ReadActive_ = false;
        }

        if (WriteActive_) {
            Failed_ = true;
            WritePromise_.Set(Error_);
            WriteActive_ = false;
        }
    }

    template <class T>
    void HandleUnderlyingIOResult(TFuture<T> future, TCallback<void(const TErrorOr<T>&)> handler)
    {
        future.Subscribe(BIND([handler = std::move(handler), invoker = Invoker_] (const TErrorOr<T>& result) {
            GuardedInvoke(
                std::move(invoker),
                BIND(handler, result),
                BIND([=] {
                    TError error("Poller terminated");
                    handler(error);
                }));
        }));
    }

    void MaybeStartUnderlyingIO(bool sslWantRead)
    {
        if (!UnderlyingReadActive_ && sslWantRead) {
            UnderlyingReadActive_ = true;
            HandleUnderlyingIOResult(
                Underlying_->Read(InputBuffer_),
                BIND([=, this_ = MakeStrong(this)] (const TErrorOr<size_t>& result) {
                    UnderlyingReadActive_ = false;
                    if (result.IsOK()) {
                        if (result.Value() > 0) {
                            int count = BIO_write(InputBIO_, InputBuffer_.Begin(), result.Value());
                            YCHECK(count == result.Value());
                        } else {
                            BIO_set_mem_eof_return(InputBIO_, 0);
                        }
                    } else {
                        Error_ = result;
                    }

                    DoRun();
                    MaybeStartUnderlyingIO(false);
                }));
        }

        if (!UnderlyingWriteActive_ && BIO_ctrl_pending(OutputBIO_)) {
            UnderlyingWriteActive_ = true;

            int count = BIO_read(OutputBIO_, OutputBuffer_.Begin(), OutputBuffer_.Size());
            YCHECK(count > 0);

            HandleUnderlyingIOResult(
                Underlying_->Write(OutputBuffer_.Slice(0, count)),
                BIND([=, this_ = MakeStrong(this)] (const TError& result) {
                    UnderlyingWriteActive_ = false;
                    if (result.IsOK()) {
                        // Hooray!
                    } else {
                        Error_ = result;
                    }

                    DoRun();
                }));
        }
    }

    void DoRun()
    {
        CheckError();

        if (CloseRequested_ && !HandshakeInProgress_) {
            SSL_shutdown(Ssl_);
            MaybeStartUnderlyingIO(false);
        }

        // NB: We should check for an error here, because Underylying_ might have failed already, and then
        // we will loop on SSL_ERROR_WANT_READ forever.
        if (HandshakeInProgress_ && Error_.IsOK()) {
           int sslResult = SSL_do_handshake(Ssl_);
            if (sslResult == 1) {
                HandshakeInProgress_ = false;
            } else {
                int sslError = SSL_get_error(Ssl_, sslResult);
                if (sslError == SSL_ERROR_WANT_READ) {
                    MaybeStartUnderlyingIO(true);
                } else {
                    Error_ = TError("SSL_do_handshake failed")
                        << GetLastSslError();
                    YT_LOG_DEBUG(Error_, "TLS handshake failed");
                    CheckError();
                    return;
                }
            }
        }

        if (HandshakeInProgress_) {
           return;
        }

        // Second condition acts as a poor-man backpressure.
        if (WriteActive_ && !UnderlyingWriteActive_) {
            for (const auto& ref : WriteBuffer_) {
                int count = SSL_write(Ssl_, ref.Begin(), ref.Size());

                if (count < 0) {
                    Error_ = TError("SSL_write failed")
                        << GetLastSslError();
                    YT_LOG_DEBUG(Error_, "TLS write failed");
                    CheckError();
                    return;
                }

                YCHECK(count == ref.Size());
            }

            MaybeStartUnderlyingIO(false);

            WriteActive_ = false;
            WriteBuffer_.Reset();
            WritePromise_.Set();
            WritePromise_.Reset();
            --ActiveIOCount_;
        }

        if (ReadActive_) {
            int count = SSL_read(Ssl_, ReadBuffer_.Begin(), ReadBuffer_.Size());
            if (count >= 0) {
                ReadActive_ = false;
                ReadPromise_.Set(count);
                ReadPromise_.Reset();
                ReadBuffer_.Reset();
                --ActiveIOCount_;
            } else {
                int sslError = SSL_get_error(Ssl_, count);
                if (sslError == SSL_ERROR_WANT_READ) {
                    MaybeStartUnderlyingIO(true);
                } else {
                    Error_ = TError("SSL_read failed")
                        << GetLastSslError();
                    YT_LOG_DEBUG(Error_, "TLS read failed");
                    CheckError();
                    return;
                }
            }
        }
    }
};

DEFINE_REFCOUNTED_TYPE(TTlsConnection)

////////////////////////////////////////////////////////////////////////////////

class TTlsDialer
    : public IDialer
{
public:
    TTlsDialer(
        TSslContextImplPtr ctx,
        IDialerPtr dialer,
        IPollerPtr poller)
        : Ctx_(std::move(ctx))
        , Underlying_(std::move(dialer))
        , Poller_(std::move(poller))
    { }

    virtual TFuture<IConnectionPtr> Dial(const TNetworkAddress& remote) override
    {
        return Underlying_->Dial(remote).Apply(BIND([ctx = Ctx_, poller = Poller_] (const IConnectionPtr& underlying) -> IConnectionPtr {
            auto connection = New<TTlsConnection>(ctx, poller, underlying);
            connection->StartClient();
            return connection;
        }));
    }

private:
    const TSslContextImplPtr Ctx_;
    const IDialerPtr Underlying_;
    const IPollerPtr Poller_;
};

////////////////////////////////////////////////////////////////////////////////

class TTlsListener
    : public IListener
{
public:
    TTlsListener(
        TSslContextImplPtr ctx,
        IListenerPtr listener,
        IPollerPtr poller)
        : Ctx_(std::move(ctx))
        , Underlying_(std::move(listener))
        , Poller_(std::move(poller))
    { }

    const TNetworkAddress& GetAddress() const override
    {
        return Underlying_->GetAddress();
    }

    virtual TFuture<IConnectionPtr> Accept() override
    {
        return Underlying_->Accept().Apply(
            BIND([ctx = Ctx_, poller = Poller_] (const IConnectionPtr& underlying) -> IConnectionPtr {
                auto connection = New<TTlsConnection>(ctx, poller, underlying);
                connection->StartServer();
                return connection;
            }));
    }

    virtual void Shutdown() override
    {
        Underlying_->Shutdown();
    }

private:
    const TSslContextImplPtr Ctx_;
    const IListenerPtr Underlying_;
    const IPollerPtr Poller_;
};

////////////////////////////////////////////////////////////////////////////////

TSslContext::TSslContext()
    : Impl_(New<TSslContextImpl>())
{ }

void TSslContext::UseBuiltinOpenSslX509Store()
{
    SSL_CTX_set_cert_store(Impl_->Ctx, GetBuiltinOpenSslX509Store().Release());
}

void TSslContext::SetCipherList(const TString& list)
{
    if (SSL_CTX_set_cipher_list(Impl_->Ctx, list.data()) == 0) {
        THROW_ERROR_EXCEPTION("SSL_CTX_set_cipher_list failed")
            << TErrorAttribute("cipher_list", list)
            << GetLastSslError();
    }
}

void TSslContext::AddCertificateFromFile(const TString& path)
{
    if (SSL_CTX_use_certificate_file(Impl_->Ctx, path.c_str(), SSL_FILETYPE_PEM) != 1) {
        THROW_ERROR_EXCEPTION("SSL_CTX_use_certificate_file failed")
            << TErrorAttribute("path", path)
            << GetLastSslError();
    }
}

void TSslContext::AddCertificateChainFromFile(const TString& path)
{
    if (SSL_CTX_use_certificate_chain_file(Impl_->Ctx, path.c_str()) != 1) {
        THROW_ERROR_EXCEPTION("SSL_CTX_use_certificate_chain_file failed")
            << TErrorAttribute("path", path)
            << GetLastSslError();
    }
}

void TSslContext::AddPrivateKeyFromFile(const TString& path)
{
    if (SSL_CTX_use_PrivateKey_file(Impl_->Ctx, path.c_str(), SSL_FILETYPE_PEM) != 1) {
        THROW_ERROR_EXCEPTION("SSL_CTX_use_PrivateKey_file failed")
            << TErrorAttribute("path", path)
            << GetLastSslError();
    }
}

void TSslContext::AddCertificateChain(const TString& certificateChain)
{
    auto bio = BIO_new_mem_buf(certificateChain.c_str(), certificateChain.size());
    YCHECK(bio);
    auto freeBio = Finally([&] {
        BIO_free(bio);
    });

    auto certificateObject = PEM_read_bio_X509_AUX(bio, nullptr, nullptr, nullptr);
    if (!certificateObject) {
        THROW_ERROR_EXCEPTION("PEM_read_bio_X509_AUX failed")
            << GetLastSslError();
    }
    auto freeCertificate = Finally([&] {
        X509_free(certificateObject);
    });

    if (SSL_CTX_use_certificate(Impl_->Ctx, certificateObject) != 1) {
        THROW_ERROR_EXCEPTION("SSL_CTX_use_certificate failed")
            << GetLastSslError();
    }

    SSL_CTX_clear_chain_certs(Impl_->Ctx);
    while (true) {
        auto chainCertificateObject = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!chainCertificateObject) {
            int err = ERR_peek_last_error();
            if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
                ERR_clear_error();
                break;
            }

            THROW_ERROR_EXCEPTION("PEM_read_bio_X509")
                << GetLastSslError();
        }

        int result = SSL_CTX_add0_chain_cert(Impl_->Ctx, chainCertificateObject);
        if (!result) {
            X509_free(chainCertificateObject);
            THROW_ERROR_EXCEPTION("SSL_CTX_add0_chain_cert")
                << GetLastSslError();
        }
    }
}

void TSslContext::AddCertificate(const TString& certificate)
{
    auto bio = BIO_new_mem_buf(certificate.c_str(), certificate.size());
    YCHECK(bio);
    auto freeBio = Finally([&] {
        BIO_free(bio);
    });

    auto certificateObject = PEM_read_bio_X509_AUX(bio, nullptr, nullptr, nullptr);
    if (!certificateObject) {
        THROW_ERROR_EXCEPTION("PEM_read_bio_X509_AUX")
            << GetLastSslError();
    }
    auto freeCertificate = Finally([&] {
        X509_free(certificateObject);
    });

    if (SSL_CTX_use_certificate(Impl_->Ctx, certificateObject) != 1) {
        THROW_ERROR_EXCEPTION("SSL_CTX_use_certificate failed")
            << GetLastSslError();
    }
}

void TSslContext::AddPrivateKey(const TString& privateKey)
{
    auto bio = BIO_new_mem_buf(privateKey.c_str(), privateKey.size());
    YCHECK(bio);
    auto freeBio = Finally([&] {
        BIO_free(bio);
    });

    auto privateKeyObject = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    if (!privateKeyObject) {
        THROW_ERROR_EXCEPTION("PEM_read_bio_PrivateKey failed")
            << GetLastSslError();
    }
    auto freePrivateKey = Finally([&] {
        EVP_PKEY_free(privateKeyObject);
    });

    if (SSL_CTX_use_PrivateKey(Impl_->Ctx, privateKeyObject) != 1) {
        THROW_ERROR_EXCEPTION("SSL_CTX_use_PrivateKey failed")
            << GetLastSslError();
    }
}

IDialerPtr TSslContext::CreateDialer(
    const TDialerConfigPtr& config,
    const IPollerPtr& poller,
    const TLogger& logger)
{
    auto dialer = NNet::CreateDialer(config, poller, logger);
    return New<TTlsDialer>(Impl_, dialer, poller);
}

IListenerPtr TSslContext::CreateListener(
    const TNetworkAddress& at,
    const IPollerPtr& poller)
{
    auto listener = NNet::CreateListener(at, poller);
    return New<TTlsListener>(Impl_, listener, poller);
}

IListenerPtr TSslContext::CreateListener(
    const IListenerPtr& underlying,
    const IPollerPtr& poller)
{
    return New<TTlsListener>(Impl_, underlying, poller);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCrypto
