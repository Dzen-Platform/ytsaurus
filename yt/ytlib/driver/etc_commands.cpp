#include "etc_commands.h"

#include <yt/ytlib/api/client.h>

#include <yt/ytlib/ypath/rich.h>

#include <yt/build/build.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/yson/async_writer.h>

namespace NYT {
namespace NDriver {

using namespace NYPath;
using namespace NYTree;
using namespace NYson;
using namespace NSecurityClient;
using namespace NObjectClient;
using namespace NConcurrency;
using namespace NApi;
using namespace NFormats;

////////////////////////////////////////////////////////////////////////////////

void TAddMemberCommand::Execute(ICommandContextPtr context)
{
    WaitFor(context->GetClient()->AddMember(
        Group,
        Member,
        Options))
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

void TRemoveMemberCommand::Execute(ICommandContextPtr context)
{
    WaitFor(context->GetClient()->RemoveMember(
        Group,
        Member,
        Options))
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

void TParseYPathCommand::Execute(ICommandContextPtr context)
{
    auto richPath = TRichYPath::Parse(Path);
    context->ProduceOutputValue(ConvertToYsonString(richPath));
}

////////////////////////////////////////////////////////////////////////////////

void TGetVersionCommand::Execute(ICommandContextPtr context)
{
    context->ProduceOutputValue(ConvertToYsonString(GetVersion()));
}

////////////////////////////////////////////////////////////////////////////////

void TCheckPermissionCommand::Execute(ICommandContextPtr context)
{
    auto result = WaitFor(context->GetClient()->CheckPermission(
        User,
        Path.GetPath(),
        Permission,
        Options))
        .ValueOrThrow();

    context->ProduceOutputValue(BuildYsonStringFluently()
        .BeginMap()
            .Item("action").Value(result.Action)
            .DoIf(result.ObjectId.operator bool(), [&] (TFluentMap fluent) {
                fluent.Item("object_id").Value(result.ObjectId);
            })
            .DoIf(result.ObjectName.HasValue(), [&] (TFluentMap fluent) {
                fluent.Item("object_name").Value(result.ObjectName);
            })
            .DoIf(result.SubjectId.operator bool(), [&] (TFluentMap fluent) {
                fluent.Item("subject_id").Value(result.SubjectId);
            })
            .DoIf(result.SubjectName.HasValue(), [&] (TFluentMap fluent) {
                fluent.Item("subject_name").Value(result.SubjectName);
            })
        .EndMap());
}

////////////////////////////////////////////////////////////////////////////////

class TExecuteBatchCommand::TRequestExecutor
    : public TIntrinsicRefCounted
{
public:
    TRequestExecutor(
        ICommandContextPtr context,
        TRequestPtr request,
        const NRpc::TMutationId& mutationId,
        bool retry)
        : Context_(std::move(context))
        , Request_(std::move(request))
        , MutationId_(mutationId)
        , Retry_(retry)
        , SyncInput_(Input_)
        , AsyncInput_(CreateAsyncAdapter(
            &SyncInput_,
            Context_->GetClient()->GetConnection()->GetHeavyInvoker()))
        , SyncOutput_(Output_)
        , AsyncOutput_(CreateAsyncAdapter(
            &SyncOutput_,
            Context_->GetClient()->GetConnection()->GetHeavyInvoker()))
    { }

    TFuture<TYsonString> Run()
    {
        auto driver = Context_->GetDriver();
        Descriptor_ = driver->GetCommandDescriptorOrThrow(Request_->Command);

        if (Descriptor_.InputType != EDataType::Null &&
            Descriptor_.InputType != EDataType::Structured)
        {
            THROW_ERROR_EXCEPTION("Command %Qv cannot be part of a batch since it has inappropriate input type %Qlv",
                Request_->Command,
                Descriptor_.InputType);
        }

        if (Descriptor_.OutputType != EDataType::Null &&
            Descriptor_.OutputType != EDataType::Structured)
        {
            THROW_ERROR_EXCEPTION("Command %Qv cannot be part of a batch since it has inappropriate output type %Qlv",
                Request_->Command,
                Descriptor_.OutputType);
        }

        TDriverRequest driverRequest;
        driverRequest.Id = Context_->Request().Id;
        driverRequest.CommandName = Request_->Command;
        auto parameters = IAttributeDictionary::FromMap(Request_->Parameters);
        if (Descriptor_.InputType == EDataType::Structured) {
            if (!Request_->Input) {
                THROW_ERROR_EXCEPTION("Command %Qv requires input",
                    Descriptor_.CommandName);
            }
            Input_ = ConvertToYsonString(Request_->Input).Data();
            parameters->Set("input_format", TFormat(EFormatType::Yson));
            driverRequest.InputStream = AsyncInput_;
        }
        if (Descriptor_.OutputType == EDataType::Structured) {
            parameters->Set("output_format", TFormat(EFormatType::Yson));
            driverRequest.OutputStream = AsyncOutput_;
        }
        if (Descriptor_.Volatile) {
            parameters->Set("mutation_id", MutationId_);
            parameters->Set("retry", Retry_);
        }
        driverRequest.Parameters = parameters->ToMap();
        driverRequest.AuthenticatedUser = Context_->Request().AuthenticatedUser;

        return driver->Execute(driverRequest).Apply(
            BIND(&TRequestExecutor::OnResponse, MakeStrong(this)));
    }

private:
    const ICommandContextPtr Context_;
    const TRequestPtr Request_;
    const NRpc::TMutationId MutationId_;
    const bool Retry_;

    TCommandDescriptor Descriptor_;

    Stroka Input_;
    TStringInput SyncInput_;
    IAsyncInputStreamPtr AsyncInput_;

    Stroka Output_;
    TStringOutput SyncOutput_;
    IAsyncOutputStreamPtr AsyncOutput_;

    TYsonString OnResponse(const TError& error)
    {
        return BuildYsonStringFluently()
            .BeginMap()
                .DoIf(!error.IsOK(), [&] (TFluentMap fluent) {
                    fluent
                        .Item("error").Value(error);
                })
                .DoIf(error.IsOK() && Descriptor_.OutputType == EDataType::Structured, [&] (TFluentMap fluent) {
                    fluent
                        .Item("output").Value(TYsonString(Output_));
                })
            .EndMap();
    }
};

void TExecuteBatchCommand::Execute(ICommandContextPtr context)
{
    auto mutationId = Options.GetOrGenerateMutationId();

    std::vector<TCallback<TFuture<TYsonString>()>> callbacks;
    for (const auto& request : Requests) {
        auto executor = New<TRequestExecutor>(
            context,
            request,
            mutationId,
            Options.Retry);
        ++mutationId.Parts32[0];
        callbacks.push_back(BIND(&TRequestExecutor::Run, executor));
    }

    auto results = WaitFor(RunWithBoundedConcurrency(callbacks, Options.Concurrency))
        .ValueOrThrow();

    context->ProduceOutputValue(BuildYsonStringFluently()
        .DoListFor(results, [&] (TFluentList fluent, const TErrorOr<TYsonString>& result) {
            fluent.Item().Value(result.ValueOrThrow());
        }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
