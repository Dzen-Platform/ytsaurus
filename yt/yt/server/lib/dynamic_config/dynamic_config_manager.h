#pragma once

#include "config.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/core/actions/signal.h>

#include <yt/core/concurrency/public.h>

namespace NYT::NDynamicConfig {

////////////////////////////////////////////////////////////////////////////////

//! Manages dynamic configuration of a server component
//! by pulling it periodically from masters.
/*!
 *  \note
 *  Thread affinity: invoker (unless noted otherwise)
 */
template <typename TConfig>
class TDynamicConfigManagerBase
    : public TRefCounted
{
public:
    using TConfigPtr = TIntrusivePtr<TConfig>;

public:
    //! Raises when dynamic config changes.
    DEFINE_SIGNAL(void(const TConfigPtr&), ConfigUpdated);

public:
    // NB: Invoker must be serialized.
    TDynamicConfigManagerBase(
        TDynamicConfigManagerOptions options,
        TDynamicConfigManagerConfigPtr config,
        NApi::NNative::IClientPtr masterClient,
        IInvokerPtr invoker);

    //! Starts the dynamic config manager.
    void Start();

    //! Returns the list of last config update attempt errors.
    std::vector<TError> GetErrors() const;

    //! Returns orchid with config and last config update time.
    NYTree::IYPathServicePtr GetOrchidService() const;

    //! Returns |true| if dynamic config was loaded successfully
    //! at least once.
    /*!
    *  \note
    *  Thread affinity: any
    */
    bool IsConfigLoaded() const;

    //! Returns a future that becomes set when dynamic config
    //! is loaded for the first time.
    /*!
    *  \note
    *  Thread affinity: any
    */
    TFuture<void> GetConfigLoadedFuture() const;

protected:
    //! Returns the list of instance tags.
    virtual std::vector<TString> GetInstanceTags() const;

private:
    void DoUpdateConfig();

    //! Returns |true| if config was actually updated.
    //! Throws on error.
    bool TryUpdateConfig();

    void DoBuildOrchid(NYson::IYsonConsumer* consumer) const;

    const TDynamicConfigManagerOptions Options_;
    const TDynamicConfigManagerConfigPtr Config_;

    const NApi::NNative::IClientPtr MasterClient_;

    const IInvokerPtr Invoker_;
    const NConcurrency::TPeriodicExecutorPtr UpdateExecutor_;

    //! Result of the last config update attempt.
    TError Error_;
    TError UnrecognizedOptionError_;

    TInstant LastConfigUpdateTime_;

    NYTree::INodePtr AppliedConfigNode_;

    std::vector<TString> InstanceTags_;

    //! This promise becomes set when dynamic config was loaded
    //! for the first time.
    TPromise<void> ConfigLoadedPromise_ = NewPromise<void>();

    const NLogging::TLogger Logger;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::DynamicConfig

#define DYNAMIC_CONFIG_MANAGER_INL_H
#include "dynamic_config_manager-inl.h"
#undef DYNAMIC_CONFIG_MANAGER_INL_H
