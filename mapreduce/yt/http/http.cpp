#include "http.h"

#include "abortable_http_response.h"

#include <mapreduce/yt/common/config.h>
#include <mapreduce/yt/common/helpers.h>

#include <mapreduce/yt/interface/logging/log.h>

#include <library/json/json_writer.h>
#include <library/string_utils/base64/base64.h>

#include <util/generic/singleton.h>

#include <util/string/quote.h>
#include <util/string/printf.h>
#include <util/string/cast.h>
#include <util/string/builder.h>
#include <util/system/getpid.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

THttpHeader::THttpHeader(const TString& method, const TString& command, bool isApi)
    : Method(method)
    , Command(command)
    , IsApi(isApi)
{ }

void THttpHeader::AddParameter(const TString& key, TNode value, bool overwrite)
{
    auto it = Parameters.find(key);
    if (it == Parameters.end()) {
        Parameters.emplace(key, std::move(value));
    } else {
        if (overwrite) {
            it->second = std::move(value);
        } else {
            ythrow yexception() << "Duplicate key: " << key;
        }
    }
}

void THttpHeader::MergeParameters(const TNode& newParameters, bool overwrite)
{
    for (const auto& p : newParameters.AsMap()) {
        AddParameter(p.first, p.second, overwrite);
    }
}

void THttpHeader::RemoveParameter(const TString& key)
{
    Parameters.erase(key);
}

TNode THttpHeader::GetParameters() const
{
    return Parameters;
}

void THttpHeader::AddTransactionId(const TTransactionId& transactionId, bool overwrite)
{
    if (transactionId) {
        AddParameter("transaction_id", GetGuidAsString(transactionId), overwrite);
    } else {
        RemoveParameter("transaction_id");
    }
}

void THttpHeader::AddPath(const TString& path, bool overwrite)
{
    AddParameter("path", path, overwrite);
}

void THttpHeader::AddOperationId(const TOperationId& operationId, bool overwrite)
{
    AddParameter("operation_id", GetGuidAsString(operationId), overwrite);
}

void THttpHeader::AddMutationId()
{
    TGUID guid;

    // Some users use `fork()' with yt wrapper
    // (actually they use python + multiprocessing)
    // and CreateGuid is not resistant to `fork()', so spice it a little bit.
    //
    // Check IGNIETFERRO-610
    CreateGuid(&guid);
    guid.dw[2] = GetPID() ^ MicroSeconds();

    AddParameter("mutation_id", GetGuidAsString(guid), true);
}

bool THttpHeader::HasMutationId() const
{
    return Parameters.has("mutation_id");
}

void THttpHeader::SetToken(const TString& token)
{
    Token = token;
}

void THttpHeader::SetInputFormat(const TMaybe<TFormat>& format)
{
    InputFormat = format;
}

void THttpHeader::SetOutputFormat(const TMaybe<TFormat>& format)
{
    OutputFormat = format;
}

TMaybe<TFormat> THttpHeader::GetOutputFormat() const
{
    return OutputFormat;
}

void THttpHeader::SetRequestCompression(const TString& compression)
{
    RequestCompression = compression;
}

void THttpHeader::SetResponseCompression(const TString& compression)
{
    ResponseCompression = compression;
}

TString THttpHeader::GetCommand() const
{
    return Command;
}

TString THttpHeader::GetUrl() const
{
    TStringStream url;

    if (IsApi) {
        url << "/api/" << TConfig::Get()->ApiVersion << "/" << Command;
    } else {
        url << "/" << Command;
    }

    return url.Str();
}

TString THttpHeader::GetHeader(const TString& hostName, const TString& requestId, bool includeParameters) const
{
    TStringStream header;

    header << Method << " " << GetUrl() << " HTTP/1.1\r\n";
    header << "Host: " << hostName << "\r\n";
    header << "User-Agent: " << TProcessState::Get()->ClientVersion << "\r\n";

    if (!Token.Empty()) {
        header << "Authorization: OAuth " << Token << "\r\n";
    }

    if (Method == "PUT" || Method == "POST") {
        header << "Transfer-Encoding: chunked\r\n";
    }

    header << "X-YT-Correlation-Id: " << requestId << "\r\n";
    header << "X-YT-Header-Format: <format=text>yson\r\n";

    header << "Content-Encoding: " << RequestCompression << "\r\n";
    header << "Accept-Encoding: " << ResponseCompression << "\r\n";

    auto printYTHeader = [&header] (const char* headerName, const TString& value) {
        static const size_t maxHttpHeaderSize = 64 << 10;
        if (!value) {
            return;
        }
        if (value.Size() <= maxHttpHeaderSize) {
            header << headerName << ": " << value << "\r\n";
            return;
        }

        TString encoded;
        Base64Encode(value, encoded);
        auto ptr = encoded.Data();
        auto finish = encoded.Data() + encoded.Size();
        size_t index = 0;
        do {
            auto end = Min(ptr + maxHttpHeaderSize, finish);
            header << headerName << index++ << ": " <<
                TStringBuf(ptr, end) << "\r\n";
            ptr = end;
        } while (ptr != finish);
    };

    if (InputFormat) {
        printYTHeader("X-YT-Input-Format", NodeToYsonString(InputFormat->Config));
    }
    if (OutputFormat) {
        printYTHeader("X-YT-Output-Format", NodeToYsonString(OutputFormat->Config));
    }
    if (includeParameters) {
        printYTHeader("X-YT-Parameters", NodeToYsonString(Parameters));
    }

    header << "\r\n";
    return header.Str();
}

const TString& THttpHeader::GetMethod() const
{
    return Method;
}

////////////////////////////////////////////////////////////////////////////////

TAddressCache* TAddressCache::Get()
{
    return Singleton<TAddressCache>();
}

TAddressCache::TAddressPtr TAddressCache::Resolve(const TString& hostName)
{
    {
        TReadGuard guard(Lock_);
        if (auto* entry = Cache_.FindPtr(hostName)) {
            return *entry;
        }
    }

    TString host(hostName);
    ui16 port = 80;

    auto colon = hostName.find(':');
    if (colon != TString::npos) {
        port = FromString<ui16>(hostName.substr(colon + 1));
        host = hostName.substr(0, colon);
    }

    TAddressPtr entry = new TNetworkAddress(host, port);

    TWriteGuard guard(Lock_);
    Cache_.insert({hostName, entry});
    return entry;
}

////////////////////////////////////////////////////////////////////////////////

TConnectionPool* TConnectionPool::Get()
{
    return Singleton<TConnectionPool>();
}

TConnectionPtr TConnectionPool::Connect(
    const TString& hostName,
    TDuration socketTimeout)
{
    Refresh();

    if (socketTimeout == TDuration::Zero()) {
        socketTimeout = TConfig::Get()->SocketTimeout;
    }

    {
        auto guard = Guard(Lock_);
        auto now = TInstant::Now();
        auto range = Connections_.equal_range(hostName);
        for (auto it = range.first; it != range.second; ++it) {
            auto& connection = it->second;
            if (connection->DeadLine < now) {
                continue;
            }
            if (!AtomicCas(&connection->Busy, 1, 0)) {
                continue;
            }

            connection->DeadLine = now + socketTimeout;
            connection->Socket->SetSocketTimeout(socketTimeout.Seconds());
            return connection;
        }
    }

    TConnectionPtr connection(new TConnection);

    auto networkAddress = TAddressCache::Get()->Resolve(hostName);
    TSocketHolder socket(DoConnect(networkAddress));
    SetNonBlock(socket, false);

    connection->Socket.Reset(new TSocket(socket.Release()));

    connection->DeadLine = TInstant::Now() + socketTimeout;
    connection->Socket->SetSocketTimeout(socketTimeout.Seconds());

    {
        auto guard = Guard(Lock_);
        static ui32 connectionId = 0;
        connection->Id = ++connectionId;
        Connections_.insert({hostName, connection});
    }

    LOG_DEBUG("Connection #%u opened",
        connection->Id);

    return connection;
}

void TConnectionPool::Release(TConnectionPtr connection)
{
    auto socketTimeout = TConfig::Get()->SocketTimeout;
    auto newDeadline = TInstant::Now() + socketTimeout;

    {
        auto guard = Guard(Lock_);
        connection->DeadLine = newDeadline;
    }

    connection->Socket->SetSocketTimeout(socketTimeout.Seconds());
    AtomicSet(connection->Busy, 0);

    Refresh();
}

void TConnectionPool::Invalidate(
    const TString& hostName,
    TConnectionPtr connection)
{
    auto guard = Guard(Lock_);
    auto range = Connections_.equal_range(hostName);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == connection) {
            LOG_DEBUG("Connection #%u invalidated",
                connection->Id);
            Connections_.erase(it);
            return;
        }
    }
}

void TConnectionPool::Refresh()
{
    auto guard = Guard(Lock_);

    // simple, since we don't expect too many connections
    using TItem = std::pair<TInstant, TConnectionMap::iterator>;
    std::vector<TItem> sortedConnections;
    for (auto it = Connections_.begin(); it != Connections_.end(); ++it) {
        sortedConnections.emplace_back(it->second->DeadLine, it);
    }

    std::sort(
        sortedConnections.begin(),
        sortedConnections.end(),
        [] (const TItem& a, const TItem& b) -> bool {
            return a.first < b.first;
        });

    auto removeCount = static_cast<int>(Connections_.size()) - TConfig::Get()->ConnectionPoolSize;

    const auto now = TInstant::Now();
    for (const auto& item : sortedConnections) {
        const auto& mapIterator = item.second;
        auto connection = mapIterator->second;
        if (AtomicGet(connection->Busy)) {
            continue;
        }

        if (removeCount > 0) {
            Connections_.erase(mapIterator);
            LOG_DEBUG("Connection #%u closed",
                connection->Id);
            --removeCount;
            continue;
        }

        if (connection->DeadLine < now) {
            Connections_.erase(mapIterator);
            LOG_DEBUG("Connection #%u closed (timeout)",
                connection->Id);
        }
    }
}

SOCKET TConnectionPool::DoConnect(TAddressCache::TAddressPtr address)
{
    int lastError = 0;

    for (auto i = address->Begin(); i != address->End(); ++i) {
        struct addrinfo* info = &*i;

        if (TConfig::Get()->ForceIpV4 && info->ai_family != AF_INET) {
            continue;
        }

        if (TConfig::Get()->ForceIpV6 && info->ai_family != AF_INET6) {
            continue;
        }

        TSocketHolder socket(
            ::socket(info->ai_family, info->ai_socktype, info->ai_protocol));

        if (socket.Closed()) {
            lastError = LastSystemError();
            continue;
        }

        SetNonBlock(socket, true);

        if (connect(socket, info->ai_addr, info->ai_addrlen) == 0)
            return socket.Release();

        int err = LastSystemError();
        if (err == EINPROGRESS || err == EAGAIN || err == EWOULDBLOCK) {
            struct pollfd p = {
                socket,
                POLLOUT,
                0
            };
            const ssize_t n = PollD(&p, 1, TInstant::Now() + TConfig::Get()->ConnectTimeout);
            if (n < 0) {
                ythrow TSystemError(-(int)n) << "can not connect to " << info;
            }
            CheckedGetSockOpt(socket, SOL_SOCKET, SO_ERROR, err, "socket error");
            if (!err)
                return socket.Release();
        }

        lastError = err;
        continue;
    }

    ythrow TSystemError(lastError) << "can not connect to " << *address;
}

////////////////////////////////////////////////////////////////////////////////

THttpResponse::THttpResponse(
    IInputStream* socketStream,
    const TString& requestId,
    const TString& hostName)
    : HttpInput_(socketStream)
    , RequestId_(requestId)
    , HostName_(hostName)
{
    HttpCode_ = ParseHttpRetCode(HttpInput_.FirstLine());
    if (HttpCode_ == 200 || HttpCode_ == 202) {
        return;
    }

    ErrorResponse_ = TErrorResponse(HttpCode_, RequestId_);

    auto logAndSetError = [&] (const TString& rawError) {
        LOG_ERROR("RSP %s - HTTP %d - %s",
            ~RequestId_,
            HttpCode_,
            ~rawError);
        ErrorResponse_->SetRawError(rawError);
    };

    switch (HttpCode_) {
        case 401:
            logAndSetError("authentication error");
            break;

        case 429:
            logAndSetError("request rate limit exceeded");
            break;

        case 500:
            logAndSetError(TStringBuilder() << "internal error in proxy " << HostName_);
            break;

        case 503:
            logAndSetError(TStringBuilder() << "proxy " << HostName_ << " is unavailable");
            break;

        default: {
            TStringStream httpHeaders;
            httpHeaders << "HTTP headers (";
            for (const auto& header : HttpInput_.Headers()) {
                httpHeaders << header.Name() << ": " << header.Value() << "; ";
            }
            httpHeaders << ")";

            auto errorString = Sprintf("RSP %s - HTTP %d - %s",
                ~RequestId_,
                HttpCode_,
                ~httpHeaders.Str());

            LOG_ERROR(~errorString);

            if (auto parsedResponse = ParseError(HttpInput_.Headers())) {
                ErrorResponse_ = parsedResponse.GetRef();
            } else {
                ErrorResponse_->SetRawError(
                    errorString + " - X-YT-Error is missing in headers");
            }
            break;
        }
    }
}

const THttpHeaders& THttpResponse::Headers() const
{
    return HttpInput_.Headers();
}

void THttpResponse::CheckErrorResponse() const
{
    if (ErrorResponse_) {
        throw *ErrorResponse_;
    }
}

bool THttpResponse::IsExhausted() const
{
    return IsExhausted_;
}

TMaybe<TErrorResponse> THttpResponse::ParseError(const THttpHeaders& headers)
{
    for (const auto& header : headers) {
        if (header.Name() == "X-YT-Error") {
            TErrorResponse errorResponse(HttpCode_, RequestId_);
            errorResponse.ParseFromJsonError(header.Value());
            if (errorResponse.IsOk()) {
                return Nothing();
            }
            return errorResponse;
        }
    }
    return Nothing();
}

size_t THttpResponse::DoRead(void* buf, size_t len)
{
    size_t read = HttpInput_.Read(buf, len);
    if (read == 0 && len != 0) {
        // THttpInput MUST return defined (but may be empty)
        // trailers when it is exhausted.
        Y_VERIFY(HttpInput_.Trailers().Defined(),
            "trailers MUST be defined for exhausted stream");
        CheckTrailers(HttpInput_.Trailers().GetRef());
        IsExhausted_ = true;
    }
    return read;
}

size_t THttpResponse::DoSkip(size_t len)
{
    size_t skipped = HttpInput_.Skip(len);
    if (skipped == 0 && len != 0) {
        // THttpInput MUST return defined (but may be empty)
        // trailers when it is exhausted.
        Y_VERIFY(HttpInput_.Trailers().Defined(),
            "trailers MUST be defined for exhausted stream");
        CheckTrailers(HttpInput_.Trailers().GetRef());
        IsExhausted_ = true;
    }
    return skipped;
}

void THttpResponse::CheckTrailers(const THttpHeaders& trailers)
{
    if (auto errorResponse = ParseError(trailers)) {
        LOG_ERROR("RSP %s - %s",
            ~RequestId_,
            errorResponse.GetRef().what());
        ythrow errorResponse.GetRef();
    }
}

////////////////////////////////////////////////////////////////////////////////

THttpRequest::THttpRequest(const TString& hostName)
    : HostName(hostName)
{
    RequestId = CreateGuidAsString();
}

THttpRequest::~THttpRequest()
{
    if (!Connection) {
        return;
    }

    bool keepAlive = false;
    if (Input) {
        for (const auto& header : Input->Headers()) {
            if (header.Name() == "Connection" && to_lower(header.Value()) == "keep-alive") {
                keepAlive = true;
                break;
            }
        }
    }

    if (keepAlive && Input && Input->IsExhausted()) {
        // We should return to the pool only connections were HTTP response was fully read.
        // Otherwise next reader might read our remaining data and misinterpret them (YT-6510).
        TConnectionPool::Get()->Release(Connection);
    } else {
        TConnectionPool::Get()->Invalidate(HostName, Connection);
    }
}

TString THttpRequest::GetRequestId() const
{
    return RequestId;
}

void THttpRequest::Connect(TDuration socketTimeout)
{
    LOG_DEBUG("REQ %s - connect to %s",
        ~RequestId,
        ~HostName);

    Connection = TConnectionPool::Get()->Connect(HostName, socketTimeout);

    LOG_DEBUG("REQ %s - connection #%u",
        ~RequestId,
        Connection->Id);
}

THttpOutput* THttpRequest::StartRequestImpl(const THttpHeader& header, bool includeParameters)
{
    auto strHeader = header.GetHeader(HostName, RequestId, includeParameters);
    Url_ = header.GetUrl();
    LOG_DEBUG("REQ %s - %s",
        ~RequestId,
        ~Url_);

    auto outputFormat = header.GetOutputFormat();
    if (outputFormat && outputFormat->Type == EFormatType::YsonText) {
        LogResponse = true;
    }

    SocketOutput.Reset(new TSocketOutput(*Connection->Socket.Get()));
    Output.Reset(new THttpOutput(SocketOutput.Get()));
    Output->EnableKeepAlive(true);

    Output->Write(~strHeader, +strHeader);
    return Output.Get();
}

THttpOutput* THttpRequest::StartRequest(const THttpHeader& header)
{
    auto parameters = header.GetParameters();
    if (!parameters.Empty()) {
        auto parametersStr = NodeToYsonString(parameters);
        LOG_DEBUG("REQ %s - X-YT-Parameters: %s",
            ~RequestId,
            ~parametersStr);
    }
    return StartRequestImpl(header, true);
}

void THttpRequest::FinishRequest()
{
    Output->Flush();
    Output->Finish();
}

void THttpRequest::SmallRequest(const THttpHeader& header, TMaybe<TStringBuf> body)
{
    if (!body && (header.GetMethod() == "PUT" || header.GetMethod() == "POST")) {
        auto parameters = header.GetParameters();
        auto parametersStr = NodeToYsonString(parameters);
        if (!parameters.Empty()) {
            // Want to log parameters before request.
            LOG_DEBUG("REQ %s - parameters (in body): %s",
                ~RequestId,
                ~parametersStr);
        }
        auto* output = StartRequestImpl(header, false);
        output->Write(parametersStr);
        FinishRequest();
    } else {
        auto* output = StartRequest(header);
        if (body) {
            output->Write(*body);
        }
        FinishRequest();
    }
}

THttpResponse* THttpRequest::GetResponseStream()
{
    SocketInput.Reset(new TSocketInput(*Connection->Socket.Get()));
    if (TConfig::Get()->UseAbortableResponse) {
        Y_VERIFY(!Url_.empty());
        Input.Reset(new TAbortableHttpResponse(SocketInput.Get(), RequestId, HostName, Url_));
    } else {
        Input.Reset(new THttpResponse(SocketInput.Get(), RequestId, HostName));
    }
    Input->CheckErrorResponse();
    return Input.Get();
}

TString THttpRequest::GetResponse()
{
    TString result = GetResponseStream()->ReadAll();
    if (LogResponse) {
        const size_t sizeLimit = 2 << 10;
        if (result.size() > sizeLimit) {
            LOG_DEBUG("RSP %s - %s...truncated - %" PRISZT " bytes total",
                ~RequestId,
                ~result.substr(0, sizeLimit),
                result.size());
        } else {
            LOG_DEBUG("RSP %s - %s",
                ~RequestId,
                ~result);
        }
    } else {
        LOG_DEBUG("RSP %s - %" PRISZT " bytes",
            ~RequestId,
            result.size());
    }
    return result;
}

void THttpRequest::InvalidateConnection()
{
    TConnectionPool::Get()->Invalidate(HostName, Connection);
    Connection.Reset();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
