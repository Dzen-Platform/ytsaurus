#include "stdafx.h"
#include "local_changelog_store.h"
#include "changelog.h"
#include "file_changelog_dispatcher.h"
#include "config.h"
#include "private.h"

#include <core/misc/fs.h>
#include <core/misc/async_cache.h>

#include <core/logging/log.h>

#include <ytlib/hydra/hydra_manager.pb.h>

namespace NYT {
namespace NHydra {

using namespace NConcurrency;
using namespace NHydra::NProto;

////////////////////////////////////////////////////////////////////////////////

class TCachedLocalChangelog
    : public TAsyncCacheValueBase<int, TCachedLocalChangelog>
    , public IChangelog
{
public:
    explicit TCachedLocalChangelog(
        int id,
        IChangelogPtr underlyingChangelog)
        : TAsyncCacheValueBase(id)
        , UnderlyingChangelog_(underlyingChangelog)
    { }

    virtual const TChangelogMeta& GetMeta() const override
    {
        return UnderlyingChangelog_->GetMeta();
    }

    virtual int GetRecordCount() const override
    {
        return UnderlyingChangelog_->GetRecordCount();
    }

    virtual i64 GetDataSize() const override
    {
        return UnderlyingChangelog_->GetDataSize();
    }

    virtual bool IsSealed() const override
    {
        return UnderlyingChangelog_->IsSealed();
    }

    virtual TFuture<void> Append(const TSharedRef& data) override
    {
        return UnderlyingChangelog_->Append(data);
    }

    virtual TFuture<void> Flush() override
    {
        return UnderlyingChangelog_->Flush();
    }

    virtual TFuture<std::vector<TSharedRef>> Read(
        int firstRecordId,
        int maxRecords,
        i64 maxBytes) const override
    {
        return UnderlyingChangelog_->Read(firstRecordId, maxRecords, maxBytes);
    }

    virtual TFuture<void> Seal(int recordCount) override
    {
        return UnderlyingChangelog_->Seal(recordCount);
    }

    virtual TFuture<void> Unseal() override
    {
        return UnderlyingChangelog_->Unseal();
    }

    virtual TFuture<void> Close() override
    {
        return UnderlyingChangelog_->Close();
    }

private:
    IChangelogPtr UnderlyingChangelog_;

};

class TLocalChangelogStore
    : public TAsyncSlruCacheBase<int, TCachedLocalChangelog>
    , public IChangelogStore
{
public:
    TLocalChangelogStore(
        const Stroka& threadName,
        TFileChangelogStoreConfigPtr config)
        : TAsyncSlruCacheBase(config->ChangelogReaderCache)
        , Config_(config)
        , Dispatcher_(New<TFileChangelogDispatcher>(
            Config_,
            threadName))
    {
        Logger.AddTag("Path: %v", Config_->Path);
    }

    void Start()
    {
        LOG_DEBUG("Preparing changelog store");

        NFS::ForcePath(Config_->Path);
        NFS::CleanTempFiles(Config_->Path);
    }

    virtual TFuture<IChangelogPtr> CreateChangelog(int id, const TChangelogMeta& meta) override
    {
        return BIND(&TLocalChangelogStore::DoCreateChangelog, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run(id, meta);
    }

    virtual TFuture<IChangelogPtr> OpenChangelog(int id) override
    {
        return BIND(&TLocalChangelogStore::DoOpenChangelog, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run(id);
    }

    virtual TFuture<int> GetLatestChangelogId(int initialId) override
    {
        return BIND(&TLocalChangelogStore::DoGetLatestChangelogId, MakeStrong(this))
            .AsyncVia(GetHydraIOInvoker())
            .Run(initialId);
    }

private:
    const TFileChangelogStoreConfigPtr Config_;
    const TFileChangelogDispatcherPtr Dispatcher_;

    NLogging::TLogger Logger = HydraLogger;


    Stroka GetChangelogPath(int id)
    {
        return NFS::CombinePaths(
            Config_->Path,
            Format("%09d.%v", id, ChangelogExtension));
    }


    IChangelogPtr DoCreateChangelog(int id, const TChangelogMeta& meta)
    {
        auto cookie = BeginInsert(id);
        if (!cookie.IsActive()) {
            THROW_ERROR_EXCEPTION("Trying to create an already existing changelog %v",
                id);
        }

        auto path = GetChangelogPath(id);

        try {
            auto underlyingChangelog = Dispatcher_->CreateChangelog(path, meta, Config_);
            auto cachedChangelog = New<TCachedLocalChangelog>(id, underlyingChangelog);
            cookie.EndInsert(cachedChangelog);
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Error creating changelog %v",
                id);
        }

        return WaitFor(cookie.GetValue())
            .ValueOrThrow();
    }

    IChangelogPtr DoOpenChangelog(int id)
    {
        auto cookie = BeginInsert(id);
        if (cookie.IsActive()) {
            auto path = GetChangelogPath(id);
            if (!NFS::Exists(path)) {
                cookie.Cancel(TError(
                    NHydra::EErrorCode::NoSuchChangelog,
                    "No such changelog %v",
                    id));
            } else {
                try {
                    auto underlyingChangelog = Dispatcher_->OpenChangelog(path, Config_);
                    auto cachedChangelog = New<TCachedLocalChangelog>(id, underlyingChangelog);
                    cookie.EndInsert(cachedChangelog);
                } catch (const std::exception& ex) {
                    LOG_FATAL(ex, "Error opening changelog %v",
                        id);
                }
            }
        }

        auto changelogOrError = WaitFor(cookie.GetValue());
        THROW_ERROR_EXCEPTION_IF_FAILED(changelogOrError);
        return changelogOrError.Value();
    }

    int DoGetLatestChangelogId(int initialId)
    {
        int latestId = InvalidSegmentId;

        auto fileNames = NFS::EnumerateFiles(Config_->Path);
        for (const auto& fileName : fileNames) {
            auto extension = NFS::GetFileExtension(fileName);
            if (extension != ChangelogExtension)
                continue;
            auto name = NFS::GetFileNameWithoutExtension(fileName);
            try {
                int id = FromString<int>(name);
                if (id >= initialId && (id > latestId || latestId == InvalidSegmentId)) {
                    latestId = id;
                }
            } catch (const std::exception&) {
                LOG_WARNING("Found unrecognized file %Qv", fileName);
            }
        }

        return latestId;
    }

};

IChangelogStorePtr CreateLocalChangelogStore(
    const Stroka& threadName,
    TFileChangelogStoreConfigPtr config)
{
    auto store = New<TLocalChangelogStore>(
        threadName,
        config);
    store->Start();
    return store;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

