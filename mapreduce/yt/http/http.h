#pragma once

#include <mapreduce/yt/interface/common.h>
#include <mapreduce/yt/interface/errors.h>
#include <mapreduce/yt/interface/format.h>
#include <mapreduce/yt/interface/io.h>
#include <mapreduce/yt/interface/node.h>

#include <library/http/io/stream.h>

#include <util/generic/strbuf.h>
#include <util/generic/guid.h>
#include <util/network/socket.h>
#include <util/stream/input.h>
#include <util/system/mutex.h>
#include <util/system/rwlock.h>
#include <util/generic/ptr.h>

namespace NYT {

class TNode;

///////////////////////////////////////////////////////////////////////////////

class THttpHeader
{
public:
    THttpHeader(const TString& method, const TString& command, bool isApi = true);

    void AddParameter(const TString& key, TNode value, bool overwrite = false);
    void RemoveParameter(const TString& key);
    void MergeParameters(const TNode& parameters, bool overwrite = false);
    TNode GetParameters() const;

    void AddTransactionId(const TTransactionId& transactionId, bool overwrite = false);
    void AddPath(const TString& path, bool overwrite = false);
    void AddOperationId(const TOperationId& operationId, bool overwrite = false);
    void AddMutationId(bool overwrite = false);
    bool HasMutationId() const;

    void SetToken(const TString& token);

    void SetInputFormat(const TMaybe<TFormat>& format);

    void SetOutputFormat(const TMaybe<TFormat>& format);
    TMaybe<TFormat> GetOutputFormat() const;

    void SetRequestCompression(const TString& compression);
    void SetResponseCompression(const TString& compression);

    TString GetCommand() const;
    TString GetUrl() const;
    TString GetHeader(const TString& hostName, const TString& requestId) const;

private:
    const TString Method;
    const TString Command;
    const bool IsApi;

    TNode::TMapType Parameters;
    TString Token;
    TNode Attributes;

private:
    TMaybe<TFormat> InputFormat = TFormat::YsonText();
    TMaybe<TFormat> OutputFormat = TFormat::YsonText();

    TString RequestCompression = "identity";
    TString ResponseCompression = "identity";
};

////////////////////////////////////////////////////////////////////////////////

class TAddressCache
{
public:
    using TAddressPtr = TAtomicSharedPtr<TNetworkAddress>;

    static TAddressCache* Get();

    TAddressPtr Resolve(const TString& hostName);

private:
    THashMap<TString, TAddressPtr> Cache_;
    TRWMutex Lock_;
};

////////////////////////////////////////////////////////////////////////////////

struct TConnection
{
    THolder<TSocket> Socket;
    TAtomic Busy = 1;
    TInstant DeadLine;
    ui32 Id;
};

using TConnectionPtr = TAtomicSharedPtr<TConnection>;

class TConnectionPool
{
public:
    using TConnectionMap = THashMultiMap<TString, TConnectionPtr>;

    static TConnectionPool* Get();

    TConnectionPtr Connect(const TString& hostName, TDuration socketTimeout);
    void Release(TConnectionPtr connection);
    void Invalidate(const TString& hostName, TConnectionPtr connection);

private:
    void Refresh();
    static SOCKET DoConnect(TAddressCache::TAddressPtr address);

private:
    TConnectionMap Connections_;
    TMutex Lock_;
};

////////////////////////////////////////////////////////////////////////////////

//
// Input stream that handles YT-specific header/trailer errors
// and throws TErrorResponse if it finds any.
class THttpResponse
    : public IInputStream
{
public:
    // 'requestId' and 'hostName' are provided for debug reasons
    // (they will appear in some error messages).
    THttpResponse(
        IInputStream* socketStream,
        const TString& requestId,
        const TString& hostName);

    const THttpHeaders& Headers() const;

    void CheckErrorResponse() const;
    bool IsExhausted() const;

protected:
    size_t DoRead(void* buf, size_t len) override;
    size_t DoSkip(size_t len) override;

private:
    void CheckTrailers(const THttpHeaders& trailers);
    TMaybe<TErrorResponse> ParseError(const THttpHeaders& headers);

private:
    THttpInput HttpInput_;
    const TString RequestId_;
    const TString HostName_;
    int HttpCode_ = 0;
    TMaybe<TErrorResponse> ErrorResponse_;
    bool IsExhausted_ = false;
};

////////////////////////////////////////////////////////////////////////////////

class THttpRequest
{
public:
    explicit THttpRequest(const TString& hostName);
    ~THttpRequest();

    TString GetRequestId() const;

    void Connect(TDuration socketTimeout = TDuration::Zero());
    THttpOutput* StartRequest(const THttpHeader& request);
    void FinishRequest();
    THttpResponse* GetResponseStream();

    TString GetResponse();

    void InvalidateConnection();

private:
    TString HostName;
    TString RequestId;
    TString Url_;

    TConnectionPtr Connection;

    THolder<TSocketOutput> SocketOutput;
    THolder<THttpOutput> Output;

    THolder<TSocketInput> SocketInput;
    THolder<THttpResponse> Input;

    bool LogResponse = false;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
