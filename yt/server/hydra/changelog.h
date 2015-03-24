#pragma once

#include "public.h"

#include <core/misc/ref.h>

#include <core/actions/future.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

///! Represents a changelog, that is an ordered sequence of records.
struct IChangelog
    : public virtual TRefCounted
{
    //! Returns the meta.
    virtual const NProto::TChangelogMeta& GetMeta() const = 0;

    //! Returns the number of records in the changelog.
    virtual int GetRecordCount() const = 0;

    //! Returns an approximate byte size in a changelog.
    virtual i64 GetDataSize() const = 0;

    //! Returns |true| if the changelog is sealed, i.e.
    //! no further appends are possible.
    virtual bool IsSealed() const = 0;

    //! Asynchronously appends a record to the changelog.
    /*!
     *  \param recordId Record ids must be contiguous.
     *  \param data Record data
     *  \returns an asynchronous flag either indicating an error or
     *  a successful flush of the just appended record.
     */
    virtual TFuture<void> Append(const TSharedRef& data) = 0;

    //! Asynchronously flushes all previously appended records.
    /*!
     *  \returns an asynchronous flag either indicating an error or
     *  a successful flush of the just appended record.
     */
    virtual TFuture<void> Flush() = 0;

    //! Asynchronously reads records from the changelog.
    //! The call may return less records than requested.
    /*!
     *  \param firstRecordId The record id to start from.
     *  \param maxRecords A hint limits the number of records to read.
     *  \param maxBytes A hint limiting the number of bytes to read.
     *  \returns A list of records.
     */
    virtual TFuture<std::vector<TSharedRef>> Read(
        int firstRecordId,
        int maxRecords,
        i64 maxBytes) const = 0;

    //! Asynchronously seals the changelog flushing and truncating it if necessary.
    /*!
     *  \returns an asynchronous flag either indicating an error or a success.
     */
    virtual TFuture<void> Seal(int recordCount) = 0;

    //! Asynchronously resets seal flag.
    /*!
     *  Mostly useful for administrative tools.
     */
    virtual TFuture<void> Unseal() = 0;

    //! Asynchronously flushes and closes the changelog, releasing all underlying resources.
    /*
     *  Examining the result is useful when a certain underlying implementation is expected.
     *  E.g. if this changelog is backed by a local file, the returned promise is set
     *  when the file is closed.
     */
    virtual TFuture<void> Close() = 0;

};

DEFINE_REFCOUNTED_TYPE(IChangelog)

////////////////////////////////////////////////////////////////////////////////

//! Manages a collection of changelogs within a cell.
struct IChangelogStore
    : public virtual TRefCounted
{
    //! Creates a new changelog.
    virtual TFuture<IChangelogPtr> CreateChangelog(int id, const NProto::TChangelogMeta& meta) = 0;

    //! Opens an existing changelog.
    virtual TFuture<IChangelogPtr> OpenChangelog(int id) = 0;

    //! Scans for the maximum contiguous sequence of existing
    //! changelogs starting from #initialId and returns the id of the latest one.
    //! Returns |InvalidSegmentId| if the initial changelog does not exist.
    virtual TFuture<int> GetLatestChangelogId(int initialId) = 0;


    // Extension methods.

    //! Opens an existing changelog.
    //! If the requested changelog is not found then returns |nullptr|.
    TFuture<IChangelogPtr> TryOpenChangelog(int id);

};

DEFINE_REFCOUNTED_TYPE(IChangelogStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
