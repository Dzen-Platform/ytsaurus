#pragma once

#include "private.h"
#include "dynamic_state.h"

#include <yt/yt/ytlib/hive/public.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NQueueAgent {

////////////////////////////////////////////////////////////////////////////////

struct IQueueController
    : public TRefCounted
{
public:
    /*!
     *  \note Thread affinity: any.
     */
    virtual EQueueFamily GetQueueFamily() const = 0;

    /*!
     *  \note Thread affinity: any.
     */
    virtual void Start() = 0;
    /*!
     *  \note Thread affinity: any.
     */
    virtual TFuture<void> Stop() = 0;

    /*!
     *  \note Thread affinity: any.
     */
    virtual IInvokerPtr GetInvoker() const = 0;

    /*!
     *  \note Thread affinity: controller invoker.
     */
    virtual void BuildOrchid(NYTree::TFluentMap fluent) const = 0;
    /*!
     *  \note Thread affinity: controller invoker.
     */
    virtual void BuildConsumerOrchid(const NQueueClient::TCrossClusterReference& consumerRef, NYTree::TFluentMap fluent) const = 0;

    virtual void OnDynamicConfigChanged(
        const TQueueControllerDynamicConfigPtr& oldConfig,
        const TQueueControllerDynamicConfigPtr& newConfig) = 0;

    //! Return latest queue snapshot.
    /*!
     *  \note Thread affinity: any.
     */
    virtual TQueueSnapshotPtr GetLatestSnapshot() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IQueueController)

////////////////////////////////////////////////////////////////////////////////

IQueueControllerPtr CreateQueueController(
    TQueueControllerDynamicConfigPtr dynamicConfig,
    NHiveClient::TClientDirectoryPtr clientDirectory,
    NQueueClient::TCrossClusterReference queueRef,
    EQueueFamily queueFamily,
    TQueueTableRow queueRow,
    TConsumerRowMap consumerRefToRow,
    IInvokerPtr invoker);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueAgent
