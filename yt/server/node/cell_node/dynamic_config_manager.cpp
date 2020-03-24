#include "dynamic_config_manager.h"

#include "bootstrap.h"
#include "config.h"
#include "private.h"

#include <yt/server/node/data_node/master_connector.h>

#include <yt/ytlib/api/native/client.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/ytree/ypath_service.h>

namespace NYT::NCellNode {

using namespace NApi;
using namespace NConcurrency;
using namespace NLogging;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static const TLogger& Logger = CellNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TDynamicConfigManager::TDynamicConfigManager(
    TDynamicConfigManagerConfigPtr config,
    TBootstrap* bootstrap)
    : Config_(std::move(config))
    , Bootstrap_(bootstrap)
    , ControlInvoker_(Bootstrap_->GetControlInvoker())
    , Executor_(New<TPeriodicExecutor>(
        ControlInvoker_,
        BIND(&TDynamicConfigManager::DoFetchConfig, MakeWeak(this)),
        Config_->UpdatePeriod))
{ }

void TDynamicConfigManager::Start()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    if (!Config_->Enabled) {
        return;
    }

    YT_LOG_INFO("Starting dynamic config manager (UpdatePeriod: %v)",
        Config_->UpdatePeriod);

    Bootstrap_->GetMasterConnector()->SubscribePopulateAlerts(
        BIND(&TDynamicConfigManager::PopulateAlerts, MakeWeak(this)));
    Executor_->Start();

    // Fetch config for the first time before further node initialization.
    // In case of failure node will become read-only until successful config fetch.
    WaitFor(Executor_->GetExecutedEvent())
        .ThrowOnError();
}

TFuture<void> TDynamicConfigManager::Stop()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    return Executor_->Stop();
}

void TDynamicConfigManager::PopulateAlerts(std::vector<TError>* errors)
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    if (!LastError_.IsOK()) {
        errors->push_back(LastError_);
    }
    if (!LastUnrecognizedOptionError_.IsOK()) {
        errors->push_back(LastUnrecognizedOptionError_);
    }
}

NYTree::IYPathServicePtr TDynamicConfigManager::GetOrchidService()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    auto producer = BIND(&TDynamicConfigManager::DoBuildOrchid, MakeStrong(this));
    return IYPathService::FromProducer(producer);
}

bool TDynamicConfigManager::IsDynamicConfigLoaded() const
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    return ConfigLoaded_;
}

void TDynamicConfigManager::DoFetchConfig()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    YT_LOG_INFO("Fetching dynamic node config");
    bool configUpdated = false;
    try {
        configUpdated = TryFetchConfig();
    } catch (const std::exception& ex) {
        YT_LOG_WARNING(TError(ex));
        LastError_ = ex;
        return;
    }

    if (configUpdated) {
        LastError_ = TError();
    }
}

bool TDynamicConfigManager::TryFetchConfig()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    NApi::TGetNodeOptions options;
    options.ReadFrom = EMasterChannelKind::Cache;

    const auto& client = Bootstrap_->GetMasterClient();
    auto configOrError = WaitFor(client->GetNode("//sys/cluster_nodes/@config", options));
    THROW_ERROR_EXCEPTION_IF_FAILED(configOrError,
        NCellNode::EErrorCode::FailedToFetchDynamicConfig,
        "Failed to fetch dynamic config from Cypress")

    auto configNode = ConvertTo<IMapNodePtr>(configOrError.Value());
    auto nodeTagList = Bootstrap_->GetMasterConnector()->GetLocalDescriptor().GetTags();

    auto nodeTagListChanged = (nodeTagList != CurrentNodeTagList_);
    if (nodeTagListChanged) {
        YT_LOG_INFO("Node tag list has changed (OldNodeTagList: %v, NewNodeTagList: %v)",
            CurrentNodeTagList_,
            nodeTagList);
        CurrentNodeTagList_ = nodeTagList;
    }

    std::optional<int> matchingConfigIndex;
    auto configs = configNode->GetChildren();
    for (int configIndex = 0; configIndex < configs.size(); ++configIndex) {
        if (MakeBooleanFormula(configs[configIndex].first).IsSatisfiedBy(CurrentNodeTagList_)) {
            if (matchingConfigIndex) {
                THROW_ERROR_EXCEPTION(NCellNode::EErrorCode::DuplicateMatchingDynamicConfigs,
                    "Found duplicate matching dynamic configs")
                    << TErrorAttribute("first_config_filter", configs[*matchingConfigIndex].first)
                    << TErrorAttribute("second_config_filter", configs[configIndex].first);
            }

            YT_LOG_INFO("Found matching dynamic config (DynamicConfigFilter: %v)",
                configs[configIndex].first);
            matchingConfigIndex = configIndex;
        }
    }

    INodePtr newConfigNode;
    if (matchingConfigIndex) {
        newConfigNode = configs[*matchingConfigIndex].second;
    } else {
        YT_LOG_INFO("No matching config found; using empty config");
        newConfigNode = GetEphemeralNodeFactory()->CreateMap();
    }

    if (AreNodesEqual(newConfigNode, CurrentConfig_)) {
        return false;
    }

    YT_LOG_INFO("Node dynamic config has changed, reconfiguring");

    auto newConfig = New<TCellNodeDynamicConfig>();
    newConfig->SetUnrecognizedStrategy(EUnrecognizedStrategy::KeepRecursive);
    try {
        newConfig->Load(newConfigNode);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION(NCellNode::EErrorCode::InvalidDynamicConfig, "Invalid dynamic node config")
            << ex;
    }

    auto unrecognizedOptions = newConfig->GetUnrecognizedRecursively();
    if (unrecognizedOptions && unrecognizedOptions->GetChildCount() > 0 && Config_->EnableUnrecognizedOptionsAlert) {
        auto error = TError(NCellNode::EErrorCode::UnrecognizedDynamicConfigOption,
            "Found unrecognized options in dynamic config")
            << TErrorAttribute("unrecognized_options", ConvertToYsonString(unrecognizedOptions, EYsonFormat::Text));
        YT_LOG_WARNING(error);
        LastUnrecognizedOptionError_ = error;
    } else {
        LastUnrecognizedOptionError_ = TError();
    }

    ConfigLoaded_ = true;
    CurrentConfig_ = newConfigNode;
    ConfigUpdated_.Fire(newConfig);
    LastConfigUpdateTime_ = TInstant::Now();

    return true;
}

void TDynamicConfigManager::DoBuildOrchid(IYsonConsumer* consumer)
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("config").Value(CurrentConfig_)
            .Item("last_config_update_time").Value(LastConfigUpdateTime_)
        .EndMap();
}

DEFINE_REFCOUNTED_TYPE(TDynamicConfigManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellNode

