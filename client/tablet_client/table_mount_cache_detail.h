#pragma once

#include "table_mount_cache.h"

#include <yt/core/concurrency/rw_spinlock.h>

#include <yt/core/misc/async_expiring_cache.h>

namespace NYT::NTabletClient {

////////////////////////////////////////////////////////////////////////////////

class TTabletCache
{
public:
    TTabletInfoPtr Find(TTabletId tabletId);
    TTabletInfoPtr Insert(TTabletInfoPtr tabletInfo);

private:
    void RemoveExpiredEntries();

    THashMap<TTabletId, TWeakPtr<TTabletInfo>> Map_;
    NConcurrency::TReaderWriterSpinLock SpinLock_;
    TInstant LastExpiredRemovalTime_;
};

///////////////////////////////////////////////////////////////////////////////

struct TTableMountCacheKey
{
    NYPath::TYPath Path;
    NHydra::TRevision RefreshPrimaryRevision;
    NHydra::TRevision RefreshSecondaryRevision;

    TTableMountCacheKey(
        const NYPath::TYPath& path,
        NHydra::TRevision refreshPrimaryRevision = NHydra::NullRevision,
        NHydra::TRevision refreshSecondaryRevision = NHydra::NullRevision);

    operator size_t() const;
    bool operator == (const TTableMountCacheKey& other) const;
};

void FormatValue(TStringBuilderBase* builder, const TTableMountCacheKey& key, TStringBuf /*spec*/);
TString ToString(const TTableMountCacheKey& key);

///////////////////////////////////////////////////////////////////////////////

class TTableMountCacheBase
    : public ITableMountCache
    , public TAsyncExpiringCache<TTableMountCacheKey, TTableMountInfoPtr>
{
public:
    TTableMountCacheBase(TTableMountCacheConfigPtr config, const NLogging::TLogger& logger);

    virtual TFuture<TTableMountInfoPtr> GetTableInfo(const NYPath::TYPath& path) override;
    virtual TTabletInfoPtr FindTablet(TTabletId tabletId) override;
    virtual void InvalidateTablet(TTabletInfoPtr tabletInfo) override;
    virtual std::pair<bool, TTabletInfoPtr> InvalidateOnError(const TError& error) override;
    virtual void Clear();

protected:
    const TTableMountCacheConfigPtr Config_;
    const NLogging::TLogger Logger;
    TTabletCache TabletCache_;

    virtual void InvalidateTable(const TTableMountInfoPtr& tableInfo) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletClient
