#pragma once

#include "private.h"

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

//! Class that keeps information about all currently running queries.
/*!
 *  Thread affinity: ControlInvoker
 */
class TQueryRegistry
    : public TRefCounted
{
public:
    TQueryRegistry(TBootstrap* bootstrap);
    ~TQueryRegistry();

    void Register(TQueryContext* queryContext);
    void Unregister(TQueryContext* queryContext);

    void AccountPhaseCounter(TQueryContext* queryContext, EQueryPhase fromPhase, EQueryPhase toPhase);

    size_t GetQueryCount() const;
    TFuture<void> GetIdleFuture() const;

    void OnProfiling() const;

    NYTree::IYPathServicePtr GetOrchidService() const;

    void WriteStateToStderr() const;

    void SaveState();

    void Start();
    void Stop();

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TQueryRegistry);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
