#pragma once

#include "private.h"

#include <yt/yt/server/lib/io/public.h>

#include <yt/yt/ytlib/hydra/proto/hydra_manager.pb.h>

#include <yt/yt/core/misc/ref.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EFileChangelogFormat,
    (V4)
    (V5)
);

//! A fully synchronous file-based changelog implementation.
/*!
 *  The instances are single-threaded unless noted otherwise.
 *  See IChangelog for a similar partly asynchronous interface.
 */
class TSyncFileChangelog
    : public TRefCounted
{
public:
    //! Basic constructor.
    TSyncFileChangelog(
        const NIO::IIOEnginePtr& ioEngine,
        const TString& fileName,
        TFileChangelogConfigPtr config);

    ~TSyncFileChangelog();

    //! Returns the configuration passed in ctor.
    /*
     *  \note
     *  Thread affinity: any
     */
    const TFileChangelogConfigPtr& GetConfig();

    //! Returns the data file name of the changelog.
    /*
     *  \note
     *  Thread affinity: any
     */
    const TString& GetFileName() const;

    //! Opens an existing changelog.
    //! Throws an exception on failure.
    void Open();

    //! Closes the changelog.
    void Close();

    //! Creates a new changelog.
    //! Throws an exception on failure.
    void Create(EFileChangelogFormat format = EFileChangelogFormat::V5);

    //! Returns the number of records in the changelog.
    /*
     *  \note
     *  Thread affinity: any
     */
    int GetRecordCount() const;

    //! Returns an approximate byte size of a changelog.
    /*
     *  \note
     *  Thread affinity: any
     */
    i64 GetDataSize() const;

    //! Returns |true| is the changelog is open.
    /*
     *  \note
     *  Thread affinity: any
     */
    bool IsOpen() const;

    //! Synchronously appends records to the changelog.
    void Append(
        int firstRecordId,
        const std::vector<TSharedRef>& records);

    //! Flushes the changelog.
    void Flush();

    //! Synchronously reads at most #maxRecords records starting from record #firstRecordId.
    //! Stops if more than #maxBytes bytes are read.
    std::vector<TSharedRef> Read(
        int firstRecordId,
        int maxRecords,
        i64 maxBytes);

    //! Synchronously seals the changelog truncating it if necessary.
    void Truncate(int recordCount);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TSyncFileChangelog)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
