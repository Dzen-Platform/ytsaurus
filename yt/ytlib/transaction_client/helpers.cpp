#include "helpers.h"
#include "transaction_manager.h"

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NTransactionClient {

using namespace NApi;
using namespace NRpc;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

void SetTransactionId(IClientRequestPtr request, ITransactionPtr transaction)
{
    NCypressClient::SetTransactionId(
        request,
        transaction ? transaction->GetId() : NullTransactionId);
}

std::pair<TInstant, TInstant> TimestampToInstant(TTimestamp timestamp)
{
    auto lo = TInstant::Seconds(timestamp >> TimestampCounterWidth);
    auto hi = lo + TDuration::Seconds(1);
    return std::make_pair(lo, hi);
}

std::pair<TTimestamp, TTimestamp> InstantToTimestamp(TInstant instant)
{
    auto lo = instant.Seconds() << TimestampCounterWidth;
    auto hi = lo + (1 << TimestampCounterWidth);
    return std::make_pair(lo, hi);
}

std::pair<TDuration, TDuration> TimestampDiffToDuration(TTimestamp loTimestamp, TTimestamp hiTimestamp)
{
    YASSERT(loTimestamp <= hiTimestamp);
    auto loInstant = TimestampToInstant(loTimestamp);
    auto hiInstant = TimestampToInstant(hiTimestamp);
    return std::make_pair(
        hiInstant.first >= loInstant.second ? hiInstant.first - loInstant.second : TDuration::Zero(),
        hiInstant.second - loInstant.first);
}

TTransactionId MakeTabletTransactionId(
    EAtomicity atomicity,
    TCellTag cellTag,
    TTimestamp startTimestamp,
    ui32 hash)
{
    EObjectType type;
    switch (atomicity) {
        case EAtomicity::Full:
            type = EObjectType::AtomicTabletTransaction;
            break;
        case EAtomicity::None:
            type = EObjectType::NonAtomicTabletTransaction;
            break;
        default:
            YUNREACHABLE();
    }

    return MakeId(
        type,
        cellTag,
        static_cast<ui64>(startTimestamp),
        hash);
}

TTimestamp TimestampFromTransactionId(const TTransactionId& id)
{
    return TTimestamp(CounterFromId(id));
}

EAtomicity AtomicityFromTransactionId(const TTransactionId& id)
{
    switch (TypeFromId(id)) {
        case EObjectType::Transaction:
        case EObjectType::AtomicTabletTransaction:
            return EAtomicity::Full;

        case EObjectType::NonAtomicTabletTransaction:
            return EAtomicity::None;

        default:
            YUNREACHABLE();
    }
}

void ValidateTabletTransactionId(const TTransactionId& id)
{
    auto type = TypeFromId(id);
    if (type != EObjectType::Transaction &&
        type != EObjectType::AtomicTabletTransaction &&
        type != EObjectType::NonAtomicTabletTransaction)
    {
        THROW_ERROR_EXCEPTION("Transaction %v cannot be accepted by tablets since is has invalid type",
            id);
    }
}

void ValidateMasterTransactionId(const TTransactionId& id)
{
    auto type = TypeFromId(id);
    if (type != EObjectType::Transaction &&
        type != EObjectType::NestedTransaction)
    {
        THROW_ERROR_EXCEPTION("Transaction %v cannot be accepted by masters since is has invalid type",
            id);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionClient
} // namespace NYT

