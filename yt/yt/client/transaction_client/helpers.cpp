#include "helpers.h"

#include <yt/client/object_client/helpers.h>

#include <yt/core/misc/error.h>

namespace NYT::NTransactionClient {

using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

bool IsMasterTransactionId(TTransactionId id)
{
    auto type = TypeFromId(id);
    // NB: Externalized transactions are for internal use only.
    return type == NObjectClient::EObjectType::Transaction ||
        type == NObjectClient::EObjectType::NestedTransaction ||
        type == EObjectType::UploadTransaction ||
        type == EObjectType::UploadNestedTransaction;
}

void ValidateTabletTransactionId(TTransactionId id)
{
    auto type = TypeFromId(id);
    if (type != EObjectType::Transaction &&
        type != EObjectType::AtomicTabletTransaction &&
        type != EObjectType::NonAtomicTabletTransaction)
    {
        THROW_ERROR_EXCEPTION("%v is not a valid tablet transaction id",
            id);
    }
}

void ValidateMasterTransactionId(TTransactionId id)
{
    if (!IsMasterTransactionId(id)) {
        THROW_ERROR_EXCEPTION("%v is not a valid master transaction id",
            id);
    }
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
    YT_ASSERT(loTimestamp <= hiTimestamp);
    auto loInstant = TimestampToInstant(loTimestamp);
    auto hiInstant = TimestampToInstant(hiTimestamp);
    return std::make_pair(
        hiInstant.first >= loInstant.second ? hiInstant.first - loInstant.second : TDuration::Zero(),
        hiInstant.second - loInstant.first);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionClient
