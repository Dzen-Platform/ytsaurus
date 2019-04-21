#include "context.h"
#include "api.h"
#include "http_authenticator.h"
#include "coordinator.h"
#include "helpers.h"
#include "formats.h"
#include "compression.h"
#include "private.h"
#include "config.h"

#include <yt/core/json/json_writer.h>
#include <yt/core/json/config.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/http/http.h>
#include <yt/core/http/helpers.h>

#include <yt/core/rpc/authenticator.h>

#include <util/string/ascii.h>
#include <util/string/strip.h>

namespace NYT::NHttpProxy {

using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NHttp;
using namespace NDriver;
using namespace NFormats;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

TContext::TContext(
    const TApiPtr& api,
    const IRequestPtr& req,
    const IResponseWriterPtr& rsp)
    : Api_(api)
    , Request_(req)
    , Response_(rsp)
    , Logger(TLogger(HttpProxyLogger).AddTag("RequestId: %v", req->GetRequestId()))
{
    DriverRequest_.Id = RandomNumber<ui64>();
}

bool TContext::TryPrepare()
{
    if (auto trace = NTracing::GetCurrentTraceContext()) {
        if (Api_->GetConfig()->ForceTracing) {
            trace->SetSampled();
        }
    }

    ProcessDebugHeaders(Request_, Response_, Api_->GetCoordinator());

    if (auto correlationId = Request_->GetHeaders()->Find("X-YT-Correlation-ID")) {
        Logger.AddTag("CorrelationId: %v", *correlationId);
    }

    return
        TryParseRequest() &&
        TryParseCommandName() &&
        TryParseUser() &&
        TryGetDescriptor() &&
        TryCheckMethod() &&
        TryCheckAvailability() &&
        TryRedirectHeavyRequests() &&
        TryGetHeaderFormat() &&
        TryGetInputFormat() &&
        TryGetInputCompression() &&
        TryGetOutputFormat() &&
        TryGetOutputCompression() &&
        TryAcquireConcurrencySemaphore();
}

bool TContext::TryParseRequest()
{
    auto userAgent = Request_->GetHeaders()->Find("User-Agent");
    if (userAgent && userAgent->find("Trident") != TString::npos) {
        // XXX(sandello): IE is bugged; it fails to parse request with trailing
        // headers that include colons. Remarkable.
        OmitTrailers_ = true;
    }

    if (Request_->GetHeaders()->Find("X-YT-Omit-Trailers")) {
        OmitTrailers_ = true;
    }

    return true;
}

bool TContext::TryParseCommandName()
{
    auto versionedName = TString(Request_->GetUrl().Path);
    versionedName.to_lower();

    if (versionedName == "/api" || versionedName == "/api/") {
        Response_->SetStatus(EStatusCode::OK);
        DispatchJson([&] (auto consumer) {
            BuildYsonFluently(consumer)
                .BeginList()
                    .Item().Value("v3")
                    .Item().Value("v4")
                .EndList();
        });
        return false;
    }

    if (versionedName.StartsWith("/api/v3")) {
        ApiVersion_ = 3;
    } else if (versionedName.StartsWith("/api/v4")) {
        ApiVersion_ = 4;
    } else {
        THROW_ERROR_EXCEPTION("Unsupported API version %Qv", versionedName);
    }

    TStringBuf commandName = versionedName;
    commandName.Skip(7);
    if (commandName == "" || commandName == "/") {
        if (*ApiVersion_ == 3) {
            Response_->SetStatus(EStatusCode::OK);
            DispatchJson([this] (auto consumer) {
                BuildYsonFluently(consumer)
                    .Value(Api_->GetDriverV3()->GetCommandDescriptors());
            });
        } else if (*ApiVersion_ == 4) {
            Response_->SetStatus(EStatusCode::OK);
            DispatchJson([this] (auto consumer) {
                BuildYsonFluently(consumer)
                    .Value(Api_->GetDriverV4()->GetCommandDescriptors());
            });
        }

        return false;
    }

    if (commandName[0] != '/') {
        DispatchNotFound("Malformed command name");
        return false;
    }

    commandName.Skip(1);
    for (auto c : commandName) {
        if (c != '_' && !IsAsciiAlpha(c)) {
            DispatchNotFound("Malformed command name");
            return false;
        }
    }
    DriverRequest_.CommandName = commandName;

    return true;
}

bool TContext::TryParseUser()
{
    // NB: This function is the only thing protecting cluster from
    // unauthorized requests. Please write code without bugs.

    auto authResult = Api_->GetHttpAuthenticator()->Authenticate(Request_);
    if (!authResult.IsOK()) {
        YT_LOG_DEBUG(authResult, "Authentication error");

        if (authResult.FindMatching(NRpc::EErrorCode::InvalidCredentials)) {
            Response_->SetStatus(EStatusCode::Unauthorized);
        } else if (authResult.FindMatching(NRpc::EErrorCode::InvalidCsrfToken)) {
            Response_->SetStatus(EStatusCode::Unauthorized);
        } else {
            Response_->SetStatus(EStatusCode::ServiceUnavailable);
        }

        FillYTErrorHeaders(Response_, TError(authResult));
        DispatchJson([&] (auto consumer) {
            BuildYsonFluently(consumer)
                .Value(TError(authResult));
        });
        return false;
    }

    Auth_ = authResult.Value();

    if (DriverRequest_.CommandName == "ping_tx" || DriverRequest_.CommandName == "parse_ypath") {
        DriverRequest_.AuthenticatedUser = Auth_->Login;
        return true;
    }

    if (Api_->IsUserBannedInCache(Auth_->Login)) {
        Response_->SetStatus(EStatusCode::Forbidden);
        ReplyFakeError(Format("User %Qv is banned", Auth_->Login));
        return false;
    }

    DriverRequest_.AuthenticatedUser = Auth_->Login;
    return true;
}

bool TContext::TryGetDescriptor()
{
    if (*ApiVersion_ == 3) {
        Descriptor_ = Api_->GetDriverV3()->FindCommandDescriptor(DriverRequest_.CommandName);
    } else {
        Descriptor_ = Api_->GetDriverV4()->FindCommandDescriptor(DriverRequest_.CommandName);
    }

    if (!Descriptor_) {
        DispatchNotFound(Format("Command %Qv is not registered", DriverRequest_.CommandName));
        return false;
    }

    return true;
}

bool TContext::TryCheckMethod()
{
    EMethod expectedMethod;
    if (Descriptor_->InputType != NFormats::EDataType::Null) {
        expectedMethod = EMethod::Put;
    } else if (Descriptor_->Volatile) {
        expectedMethod = EMethod::Post;
    } else {
        expectedMethod = EMethod::Get;
    }

    if (Request_->GetMethod() != expectedMethod) {
        Response_->SetStatus(EStatusCode::MethodNotAllowed);
        Response_->GetHeaders()->Set("Allow", TString(ToHttpString(expectedMethod)));
        ReplyFakeError(Format("Command %Qv have to be executed with the %Qv HTTP method",
            Descriptor_->CommandName,
            ToHttpString(expectedMethod)));

        return false;
    }

    return true;
}

bool TContext::TryCheckAvailability()
{
    if (Api_->GetCoordinator()->IsBanned()) {
        DispatchUnavailable("60", "This proxy is banned");
        return false;
    }

    return true;
}

bool TContext::TryRedirectHeavyRequests()
{
    bool suppressRedirect = Request_->GetHeaders()->Find("X-YT-Suppress-Redirect");
    bool isBrowserRequest = Request_->GetHeaders()->Find("Cookie");
    if (Descriptor_->Heavy &&
        !Api_->GetCoordinator()->CanHandleHeavyRequests() &&
        !suppressRedirect &&
        !isBrowserRequest)
    {
        if (Descriptor_->InputType != NFormats::EDataType::Null) {
            DispatchUnavailable("60", "Control proxy may not serve heavy requests with input data");
            return false;
        }

        RedirectToDataProxy(Request_, Response_, Api_->GetCoordinator());
        return false;
    }

    return true;
}

bool TContext::TryGetHeaderFormat()
{
    auto headerFormat = Request_->GetHeaders()->Find("X-YT-Header-Format");
    if (headerFormat) {
        try {
            TYsonString header(StripString(*headerFormat));
            HeadersFormat_ = ConvertTo<TFormat>(header);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Unable to parse X-YT-Header-Format header")
                << ex;
        }
    } else {
        HeadersFormat_ = EFormatType::Json;
    }

    return true;
}

bool TContext::TryGetInputFormat()
{
    try {
        auto header = GatherHeader(Request_->GetHeaders(), "X-YT-Input-Format");
        if (header) {
            InputFormat_ = ConvertTo<TFormat>(ConvertBytesToNode(*header, *HeadersFormat_));
            return true;
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Unable to parse X-YT-Input-Format header")
            << ex;
    }

    auto contentTypeHeader = Request_->GetHeaders()->Find("Content-Type");
    if (contentTypeHeader) {
        auto contentType = StripString(*contentTypeHeader);
        InputFormat_ = MimeTypeToFormat(contentType);
        if (InputFormat_) {
            return true;
        }
    }

    InputFormat_ = GetDefaultFormatForDataType(Descriptor_->InputType);
    return true;
}

bool TContext::TryGetInputCompression()
{
    auto header = Request_->GetHeaders()->Find("Content-Encoding");
    if (header) {
        auto compression = StripString(*header);
        if (!IsCompressionSupported(compression)) {
            Response_->SetStatus(EStatusCode::UnsupportedMediaType);
            ReplyFakeError("Unsupported Content-Encoding");
            return false;
        }

        InputContentEncoding_ = compression;
    } else {
        InputContentEncoding_ = IdentityContentEncoding;
    }

    return true;
}

bool TContext::TryGetOutputFormat()
{
    if (Descriptor_->OutputType == NFormats::EDataType::Null ||
        Descriptor_->OutputType == NFormats::EDataType::Binary)
    {
        OutputFormat_ = EFormatType::Yson;
        return true;
    }

    try {
        auto header = GatherHeader(Request_->GetHeaders(), "X-YT-Output-Format");
        if (header) {
            OutputFormat_ = ConvertTo<TFormat>(ConvertBytesToNode(*header, *HeadersFormat_));
            return true;
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Unable to parse X-YT-Output-Format header")
            << ex;
    }

    auto acceptHeader = Request_->GetHeaders()->Find("Accept");
    if (acceptHeader) {
        auto acceptedType = GetBestAcceptedType(Descriptor_->OutputType, StripString(*acceptHeader));
        if (acceptedType) {
            OutputFormat_ = MimeTypeToFormat(*acceptedType);
        }
    }

    if (!OutputFormat_) {
        OutputFormat_ = GetDefaultFormatForDataType(Descriptor_->OutputType);
    }

    return true;
}

bool TContext::TryGetOutputCompression()
{
    auto acceptEncodingHeader = Request_->GetHeaders()->Find("Accept-Encoding");
    if (acceptEncodingHeader) {
        auto contentEncoding = GetBestAcceptedEncoding(*acceptEncodingHeader);

        if (!contentEncoding.IsOK()) {
            Response_->SetStatus(EStatusCode::UnsupportedMediaType);
            ReplyError(contentEncoding);
            return false;
        }

        OutputContentEncoding_ = contentEncoding.Value();
    } else {
        OutputContentEncoding_ = IdentityContentEncoding;
    }

    return true;
}

bool TContext::TryAcquireConcurrencySemaphore()
{
    SemaphoreGuard_ = Api_->AcquireSemaphore(DriverRequest_.AuthenticatedUser, DriverRequest_.CommandName);
    if (!SemaphoreGuard_) {
        DispatchUnavailable(
            "60",
            "There are too many concurrent requests being served at the moment; "
            "please try another proxy or try again later");
        return false;
    }

    return true;
}

void TContext::CaptureParameters()
{
    DriverRequest_.Parameters = BuildYsonNodeFluently()
        .BeginMap()
            .Item("input_format").Value(InputFormat_)
            .Item("output_format").Value(OutputFormat_)
        .EndMap()->AsMap();

    try {
        auto queryParams = ParseQueryString(Request_->GetUrl().RawQuery);
        FixupNodesWithAttributes(queryParams);
        DriverRequest_.Parameters = PatchNode(DriverRequest_.Parameters, std::move(queryParams))->AsMap();
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Unable to parse parameters from query string") << ex;
    }

    try {
        auto header = GatherHeader(Request_->GetHeaders(), "x-yt-parameters");
        if (header) {
            TMemoryInput stream(header->data(), header->size());
            auto fromHeaders = ConvertToNode(CreateProducerForFormat(
                *HeadersFormat_,
                EDataType::Structured,
                &stream));

            DriverRequest_.Parameters = PatchNode(DriverRequest_.Parameters, fromHeaders)->AsMap();
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Unable to parse parameters from headers") << ex;
    }

    if (Request_->GetMethod() == EMethod::Post) {
        auto body = Request_->ReadAll();
        if (body.Size() == 0) {
            return;
        }

        if (InputContentEncoding_ != IdentityContentEncoding) {
            THROW_ERROR_EXCEPTION("Content-Encoding not supported in POST body");
        }

        TMemoryInput stream(body.Begin(), body.Size());
        auto fromBody = ConvertToNode(CreateProducerForFormat(
            *InputFormat_,
            EDataType::Structured,
            &stream));

        DriverRequest_.Parameters = PatchNode(DriverRequest_.Parameters, fromBody)->AsMap();
    }
}

void TContext::SetETagRevision()
{
    auto etagHeader = Request_->GetHeaders()->Find("If-None-Match");
    if (etagHeader) {
        IfNoneMatch_ = FromString<ui64>(*etagHeader);
        DriverRequest_.Parameters->AsMap()->AddChild("etag_revision", ConvertToNode(*etagHeader));
    }
}

void TContext::SetContentDispositionAndMimeType()
{
    TString disposition = "attachment";
    if (Descriptor_->Heavy) {
        TString filename;
        if (Descriptor_->CommandName == "download" ||
            Descriptor_->CommandName == "read_table" ||
            Descriptor_->CommandName == "read_file"
        ) {
            if (auto path = DriverRequest_.Parameters->FindChild("path")) {
                filename = "yt_" + path->GetValue<TString>();
            }
        } else if (Descriptor_->CommandName == "get_job_stderr") {
            auto operationId = DriverRequest_.Parameters->FindChild("operation_id");
            auto jobId = DriverRequest_.Parameters->FindChild("job_id");

            disposition = "inline";
            if (operationId && jobId) {
                filename = Format("job_stderr_%v_%v",
                    operationId->GetValue<TString>(),
                    jobId->GetValue<TString>());
            }
        } else if (Descriptor_->CommandName == "get_job_fail_context") {
            auto operationId = DriverRequest_.Parameters->FindChild("operation_id");
            auto jobId = DriverRequest_.Parameters->FindChild("job_id");

            disposition = "inline";
            if (operationId && jobId) {
                filename = Format("fail_context_%v_%v",
                    operationId->GetValue<TString>(),
                    jobId->GetValue<TString>());
            }
        }

        if (auto passedFilename = DriverRequest_.Parameters->FindChild("filename")) {
            filename = passedFilename->GetValue<TString>();
        }

        for (size_t i = 0; i < filename.size(); ++i) {
            if (!std::isalnum(filename[i]) && filename[i] != '.') {
                filename[i] = '_';
            }
        }

        if (filename.Contains("sys_operations") && filename.Contains("stderr")) {
            disposition = "inline";
        }

        if (auto passedDisposition = DriverRequest_.Parameters->FindChild("disposition")) {
            auto sanitizedDisposition = passedDisposition->GetValue<TString>();
            sanitizedDisposition.to_lower();
            if (sanitizedDisposition == "inline" && sanitizedDisposition == "attachment") {
                disposition = sanitizedDisposition;
            }
        }

        if (!filename.empty()) {
            disposition += "; filename=\"" + filename + "\"";
        }

        Response_->GetHeaders()->Set("Content-Disposition", disposition);
    }

    if (Descriptor_->OutputType == EDataType::Binary) {
        if (disposition.StartsWith("inline")) {
            ContentType_ = "text/plain; charset=\"utf-8\"";
        } else {
            ContentType_ = "application/octet-stream";
        }
    } else if (Descriptor_->OutputType == EDataType::Null) {
        return;
    } else {
        ContentType_ = FormatToMime(*OutputFormat_);
    }
}

void TContext::LogRequest()
{
    DriverRequest_.Id = Request_->GetRequestId();
    YT_LOG_INFO("Gathered request parameters (RequestId: %v, Command: %v, User: %v, Parameters: %v, InputFormat: %v, InputCompression: %v, OutputFormat: %v, OutputCompression: %v)",
        Request_->GetRequestId(),
        Descriptor_->CommandName,
        DriverRequest_.AuthenticatedUser,
        ConvertToYsonString(
            HideSecretParameters(Descriptor_->CommandName, DriverRequest_.Parameters),
            EYsonFormat::Text).GetData(),
        ConvertToYsonString(InputFormat_, EYsonFormat::Text).GetData(),
        InputContentEncoding_,
        ConvertToYsonString(OutputFormat_, EYsonFormat::Text).GetData(),
        OutputContentEncoding_);
}

void TContext::SetupInputStream()
{
    if (IdentityContentEncoding == InputContentEncoding_) {
        DriverRequest_.InputStream = CreateCopyingAdapter(Request_);
    } else {
        DriverRequest_.InputStream = CreateDecompressingAdapter(Request_, *InputContentEncoding_);
    }
}

void TContext::SetupOutputStream()
{
    // NB(psushin): This is an ugly hack for a long-running command with structured output - YT-9713.
    // Remove once framing is implemented - YT-9838.
    if (Descriptor_->CommandName != "get_table_columnar_statistics" && (
        Descriptor_->OutputType == EDataType::Null ||
        Descriptor_->OutputType == EDataType::Structured))
    {
        MemoryOutput_ = New<TSharedRefOutputStream>();
        DriverRequest_.OutputStream = MemoryOutput_;
    } else {
        DriverRequest_.OutputStream = Response_;
    }

    if (IdentityContentEncoding != OutputContentEncoding_) {
        DriverRequest_.OutputStream = CreateCompressingAdapter(
            DriverRequest_.OutputStream,
            *OutputContentEncoding_);
    }
}

void TContext::SetupOutputParameters()
{
    CreateBuildingYsonConsumer(&OutputParametersConsumer_, EYsonType::Node);
    OutputParametersConsumer_->OnBeginMap();
    DriverRequest_.ResponseParametersConsumer = OutputParametersConsumer_.get();
    DriverRequest_.ParametersFinishedCallback = [this, weakThis = MakeWeak(this)] {
        auto strongThis = weakThis.Lock();
        if (!strongThis) {
            return;
        }

        OutputParametersConsumer_->OnEndMap();
        OutputParameters_ = OutputParametersConsumer_->Finish()->AsMap();
        OnOutputParameters();
    };
}

void TContext::AddHeaders()
{
    auto headers = Response_->GetHeaders();

    if (ContentType_) {
        headers->Set("Content-Type", *ContentType_);
    }

    if (OutputContentEncoding_) {
        headers->Set("Content-Encoding", *OutputContentEncoding_);
        headers->Add("Vary", "Content-Encoding");
    }

    if (!OmitTrailers_) {
        headers->Set("Trailer", "X-YT-Error, X-YT-Response-Code, X-YT-Response-Message");
    }
}

void TContext::SetError(const TError& error)
{
    Error_ = error;
}

void TContext::FinishPrepare()
{
    CaptureParameters();
    SetContentDispositionAndMimeType();
    SetETagRevision();
    LogRequest();
    SetupInputStream();
    SetupOutputStream();
    SetupOutputParameters();
    AddHeaders();
}

void TContext::Run()
{
    Response_->SetStatus(EStatusCode::OK);
    if (*ApiVersion_ == 4) {
        WaitFor(Api_->GetDriverV4()->Execute(DriverRequest_))
            .ThrowOnError();
    } else {
        WaitFor(Api_->GetDriverV3()->Execute(DriverRequest_))
            .ThrowOnError();
    }

    if (MemoryOutput_) {
        WaitFor(DriverRequest_.OutputStream->Close())
            .ThrowOnError();
        Response_->GetHeaders()->Remove("Trailer");
        WaitFor(Response_->WriteBody(MergeRefsToRef<TDefaultSharedBlobTag>(MemoryOutput_->GetRefs())))
            .ThrowOnError();
    } else {
        WaitFor(DriverRequest_.OutputStream->Close())
            .ThrowOnError();

        WaitFor(Response_->Close())
            .ThrowOnError();
    }
}

TSharedRef DumpError(const TError& error)
{
    TString delimiter;
    delimiter.append('\n');
    delimiter.append(80, '=');
    delimiter.append('\n');

    TString errorMessage;
    TStringOutput errorStream(errorMessage);
    errorStream << "\n";
    errorStream << delimiter;

    auto formatAttributes = CreateEphemeralAttributes();
    formatAttributes->SetYson("format", TYsonString("pretty"));

    auto consumer = CreateConsumerForFormat(
        TFormat(EFormatType::Json, formatAttributes.get()),
        EDataType::Structured,
        &errorStream);

    Serialize(error, consumer.get());
    consumer->Flush();

    errorStream << delimiter;
    errorStream.Finish();

    return TSharedRef::FromString(errorMessage);
}

void TContext::Finalize()
{
    if (IsClientBuggy(Request_)) {
        try {
            while (true) {
                auto chunk = WaitFor(Request_->Read())
                    .ValueOrThrow();

                if (!chunk || chunk.Empty()) {
                    break;
                }
            }
        } catch (const std::exception& ex) { }
    }

    bool dumpErrorIntoResponse = false;
    if (DriverRequest_.Parameters) {
        auto param = DriverRequest_.Parameters->FindChild("dump_error_into_response");
        if (param) {
            dumpErrorIntoResponse = ConvertTo<bool>(param);
        }
    }

    if (!Error_.IsOK() && dumpErrorIntoResponse && DriverRequest_.OutputStream) {
        Y_UNUSED(WaitFor(DriverRequest_.OutputStream->Write(DumpError(Error_))));
        Y_UNUSED(WaitFor(DriverRequest_.OutputStream->Close()));
    } else if (!Response_->IsHeadersFlushed()) {
        Response_->GetHeaders()->Remove("Trailer");

        if (Error_.FindMatching(NSecurityClient::EErrorCode::UserBanned)) {
            Response_->SetStatus(EStatusCode::Forbidden);
            Api_->PutUserIntoBanCache(DriverRequest_.AuthenticatedUser);
        } else if (!Error_.IsOK()) {
            Response_->SetStatus(EStatusCode::BadRequest);
        }
        // TODO(prime@): More error codes.

        if (!Error_.IsOK()) {
            Response_->GetHeaders()->Remove("Content-Encoding");
            Response_->GetHeaders()->Remove("Vary");

            FillYTErrorHeaders(Response_, Error_);
            DispatchJson([&] (auto producer) {
                BuildYsonFluently(producer).Value(Error_);
            });
        }
    } else {
        if (!Error_.IsOK()) {
            FillYTErrorTrailers(Response_, Error_);
            Y_UNUSED(WaitFor(Response_->Close()));
        }
    }

    Api_->IncrementProfilingCounters(
        DriverRequest_.AuthenticatedUser,
        DriverRequest_.CommandName,
        Response_->GetStatus(),
        Error_.GetCode(),
        TInstant::Now() - StartTime_,
        Request_->GetReadByteCount(),
        Response_->GetWriteByteCount());
}

template <class TJsonProducer>
void TContext::DispatchJson(const TJsonProducer& producer)
{
    ReplyJson(Response_, [&] (NYson::IYsonConsumer* consumer) {
        producer(consumer);
    });
}

void TContext::DispatchUnauthorized(const TString& scope, const TString& message)
{
    Response_->SetStatus(EStatusCode::Unauthorized);
    Response_->GetHeaders()->Set("WWW-Authenticate", scope);
    ReplyFakeError(message);
}

void TContext::DispatchUnavailable(const TString& retryAfter, const TString& message)
{
    Response_->SetStatus(EStatusCode::ServiceUnavailable);
    Response_->GetHeaders()->Set("Retry-After", retryAfter);
    ReplyFakeError(message);
}

void TContext::DispatchNotFound(const TString& message)
{
    Response_->SetStatus(EStatusCode::NotFound);
    ReplyFakeError(message);
}

void TContext::ReplyError(const TError& error)
{
    YT_LOG_DEBUG(error, "Request finished with error");
    NHttpProxy::ReplyError(Response_, error);
}

void TContext::ReplyFakeError(const TString& message)
{
    ReplyError(TError(message));
}

void TContext::OnOutputParameters()
{
    if (auto revision = OutputParameters_->FindChild("revision")) {
        Response_->GetHeaders()->Add("ETag", ToString(revision->AsUint64()->GetValue()));
        if (IfNoneMatch_ && *IfNoneMatch_ == revision->AsUint64()->GetValue()) {
            Response_->SetStatus(EStatusCode::NotModified);
        }
    }

    TString headerValue;
    TStringOutput stream(headerValue);
    auto consumer = CreateConsumerForFormat(*HeadersFormat_, EDataType::Structured, &stream);
    Serialize(OutputParameters_, consumer.get());
    consumer->Flush();

    Response_->GetHeaders()->Add("X-YT-Response-Parameters", headerValue);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
