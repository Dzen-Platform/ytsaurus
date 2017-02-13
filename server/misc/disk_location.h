#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/core/logging/log.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TDiskLocation
    : public TRefCounted
{
public:
    TDiskLocation(
        TDiskLocationConfigPtr config,
        const Stroka& id,
        const NLogging::TLogger& logger);

    //! Returns |true| iff the location is enabled.
    bool IsEnabled() const;

protected:
    mutable NLogging::TLogger Logger;
    std::atomic<bool> Enabled_ = {false};

    void ValidateMinimumSpace() const;

    i64 GetTotalSpace() const;

    void ValidateEnabled() const;

private:
    const TDiskLocationConfigPtr Config_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

