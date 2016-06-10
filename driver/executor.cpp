#include "executor.h"
#include "preprocess.h"

#include <yt/server/job_proxy/config.h>

#include <yt/ytlib/driver/command.h>
#include <yt/ytlib/driver/driver.h>

#include <yt/build/build.h>

#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/assert.h>
#include <yt/core/misc/fs.h>

#include <yt/core/tracing/trace_context.h>
#include <yt/core/tracing/trace_manager.h>

#include <yt/core/yson/format.h>
#include <yt/core/yson/tokenizer.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NDriver {

using namespace NYTree;
using namespace NYson;
using namespace NScheduler;
using namespace NRpc;
using namespace NFormats;
using namespace NTransactionClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const char* UserConfigFileName = ".ytdriver.conf";
static const char* SystemConfigFileName = "ytdriver.conf";
static const char* SystemConfigPath = "/etc/";
static const char* ConfigEnvVar = "YT_CONFIG";

////////////////////////////////////////////////////////////////////////////////

TExecutor::TExecutor()
    : CmdLine("Command line", ' ', GetVersion())
    , ConfigArg("", "config", "configuration file", false, "", "STRING")
    , ConfigOptArg("", "config_opt", "override configuration option", false, "YPATH=YSON")
{
    CmdLine.add(ConfigArg);
    CmdLine.add(ConfigOptArg);
}

Stroka TExecutor::GetConfigFileName()
{
    Stroka fromCommandLine = ConfigArg.getValue();;
    Stroka fromEnv = Stroka(getenv(ConfigEnvVar));
    Stroka user = NFS::CombinePaths(NFS::GetHomePath(), UserConfigFileName);
    Stroka system = NFS::CombinePaths(SystemConfigPath, SystemConfigFileName);

    if (!fromCommandLine.empty()) {
        return fromCommandLine;
    }

    if (!fromEnv.empty()) {
        return fromEnv;
    }

    if (NFS::Exists(user)) {
        return user;
    }

    if (NFS::Exists(system)) {
        return system;
    }

    throw std::runtime_error(Format(
        "Configuration file cannot be found. Please specify it using one of the following methods:\n"
        "1) --config command-line option\n"
        "2) %v environment variable\n"
        "3) per-user file %Qv\n"
        "4) system-wide file %Qv",
        ConfigEnvVar,
        user,
        system));
}

void TExecutor::InitConfig()
{
    // Choose config file name.
    auto fileName = GetConfigFileName();

    // Load config into YSON tree.
    INodePtr configNode;
    try {
        TIFStream configStream(fileName);
        configNode = ConvertToNode(&configStream);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error reading configuration")
            << ex;
    }

    // Parse config.
    Config = New<TExecutorConfig>();
    try {
        Config->Load(configNode);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing configuration")
            << ex;
    }

    // Now convert back YSON tree to populate defaults.
    configNode = ConvertToNode(Config);

    // Patch config from command line.
    for (const auto& opt : ConfigOptArg.getValue()) {
        ApplyYPathOverride(configNode, opt);
    }

    // And finally parse it again.
    try {
        Config->Load(configNode);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing configuration")
            << ex;
    }
}

void TExecutor::Execute(const std::vector<std::string>& args)
{
    auto argsCopy = args;
    CmdLine.parse(argsCopy);

    InitConfig();

    NTracing::TTraceContextGuard guard(Config->Trace
        ? NTracing::CreateRootTraceContext()
        : NTracing::NullTraceContext);

    if (Config->Logging) {
        NLogging::TLogManager::Get()->Configure(Config->Logging);
    }
    if (Config->Tracing) {
        NTracing::TTraceManager::Get()->Configure(Config->Tracing);
    }
    TAddressResolver::Get()->Configure(Config->AddressResolver);

    Driver = CreateDriver(Config->Driver);

    DoExecute();
}

////////////////////////////////////////////////////////////////////////////////

TRequestExecutor::TRequestExecutor()
    : AuthenticatedUserArg("", "user", "user to impersonate", false, "", "STRING")
    , FormatArg("", "format", "format (both input and output)", false, "", "YSON")
    , InputFormatArg("", "in_format", "input format", false, "", "YSON")
    , OutputFormatArg("", "out_format", "output format", false, "", "YSON")
    , OptArg("", "opt", "override command option", false, "YPATH=YSON")
    , ResponseParametersArg("", "response_parameters", "print response parameters", false)
{
    CmdLine.add(AuthenticatedUserArg);
    CmdLine.add(FormatArg);
    CmdLine.add(InputFormatArg);
    CmdLine.add(OutputFormatArg);
    CmdLine.add(OptArg);
    CmdLine.add(ResponseParametersArg);
}

void TRequestExecutor::DoExecute()
{
    auto commandName = GetCommandName();

    auto descriptor = Driver->GetCommandDescriptor(commandName);

    Stroka inputFormatString = FormatArg.getValue();
    Stroka outputFormatString = FormatArg.getValue();
    if (!InputFormatArg.getValue().empty()) {
        inputFormatString = InputFormatArg.getValue();
    }
    if (!OutputFormatArg.getValue().empty()) {
        outputFormatString = OutputFormatArg.getValue();
    }

    TNullable<TYsonString> inputFormat, outputFormat;
    if (!inputFormatString.empty()) {
        inputFormat = TYsonString(inputFormatString);
    }
    if (!outputFormatString.empty()) {
        outputFormat = TYsonString(outputFormatString);
    }
 
    TDriverRequest request;
    // GetParameters() must be called before GetInputStream()
    request.Parameters = GetParameters();
    request.CommandName = GetCommandName();

    if (AuthenticatedUserArg.isSet()) {
        request.AuthenticatedUser = AuthenticatedUserArg.getValue();
    }

    request.InputStream = CreateAsyncAdapter(GetInputStream());
    try {
        request.Parameters->AddChild(
            ConvertToNode(GetFormat(descriptor.InputType, inputFormat)),
            "input_format");
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing input format") << ex;
    }

    // Buffering is done in the upper layers.
    request.OutputStream = CreateAsyncAdapter(&Cout);
    try {
        request.Parameters->AddChild(
            ConvertToNode(GetFormat(descriptor.OutputType, outputFormat)),
            "output_format");
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing output format") << ex;
    }

    TYsonWriter ysonWriter(&Cerr, NYson::EYsonFormat::Pretty);
    if (ResponseParametersArg.getValue()) {
        request.ResponseParametersConsumer = &ysonWriter;
    }

    DoExecute(request);
}

void TRequestExecutor::DoExecute(const TDriverRequest& request)
{
    Driver->Execute(request)
        .Get()
        .ThrowOnError();
}

IMapNodePtr TRequestExecutor::GetParameters()
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();

    BuildYsonFluently(builder.get())
        .BeginMap()
            .Do(BIND(&TRequestExecutor::BuildParameters, Unretained(this)))
        .EndMap();

    auto parameters = builder->EndTree()->AsMap();
    for (const auto& opt : OptArg.getValue()) {
        ApplyYPathOverride(parameters, opt);
    }
    return parameters;
}

TFormat TRequestExecutor::GetFormat(EDataType dataType, const TNullable<TYsonString>& yson)
{
    if (yson) {
        return ConvertTo<TFormat>(yson.Get());
    }

    switch (dataType) {
        case EDataType::Null:
        case EDataType::Binary:
            return TFormat(EFormatType::Null);

        case EDataType::Structured:
            return Config->FormatDefaults->Structured;

        case EDataType::Tabular:
            return Config->FormatDefaults->Tabular;

        default:
            YUNREACHABLE();
    }
}

void TRequestExecutor::BuildParameters(IYsonConsumer* consumer)
{
    Y_UNUSED(consumer);
}

TInputStream* TRequestExecutor::GetInputStream()
{
    return &Cin;
}

////////////////////////////////////////////////////////////////////////////////

TTransactedExecutor::TTransactedExecutor(
    bool txRequired,
    bool txLabeled)
    : LabeledTxArg("", "tx", "set transaction id", txRequired, TTransactionId(), "TX_ID")
    , UnlabeledTxArg("tx", "transaction id", txRequired, TTransactionId(), "TX_ID")
    , PingAncestorTxsArg("", "ping_ancestor_txs", "ping ancestor transactions", false)
{
    CmdLine.add(txLabeled ? LabeledTxArg : UnlabeledTxArg);
    CmdLine.add(PingAncestorTxsArg);
}

void TTransactedExecutor::BuildParameters(IYsonConsumer* consumer)
{
    TNullable<TTransactionId> txId;
    if (LabeledTxArg.isSet()) {
        txId = LabeledTxArg.getValue();
    }
    if (UnlabeledTxArg.isSet()) {
        txId = UnlabeledTxArg.getValue();
    }

    if (PingAncestorTxsArg.getValue() && !txId) {
        THROW_ERROR_EXCEPTION("ping_ancestor_txs is set but no tx_id is given");
    }

    BuildYsonMapFluently(consumer)
        .DoIf(txId.HasValue(), [=] (TFluentMap fluent) {
            fluent.Item("transaction_id").Value(txId.Get());
        })
        .Item("ping_ancestor_transactions").Value(PingAncestorTxsArg.getValue());

    TRequestExecutor::BuildParameters(consumer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
