#include "address_helpers.h"

#include <yt/core/misc/address.h>

namespace NYT {

using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

TAddressMap GetLocalAddresses(const TNetworkAddressList& addresses, int port)
{
    // Аppend port number.
    TAddressMap result;
    result.reserve(addresses.size());
    for (const auto& pair : addresses) {
        YCHECK(result.emplace(pair.first, BuildServiceAddress(pair.second, port)).second);
    }

    // Add default address.
    const auto pair = result.emplace(DefaultNetworkName, TString());
    if (pair.second) {
        pair.first->second = BuildServiceAddress(GetLocalHostName(), port);
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
