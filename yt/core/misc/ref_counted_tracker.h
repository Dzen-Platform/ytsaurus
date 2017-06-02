#pragma once

#include "public.h"
#include "source_location.h"

#include <yt/core/yson/public.h>

#include <yt/core/concurrency/fork_aware_spinlock.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

// Reference tracking relies on uniqueness of std::type_info objects.
// Without uniqueness reference tracking is still functional but lacks precision
// (i. e. some types may have duplicate slots in the accumulated table).
// GCC guarantees std::type_info uniqueness starting from version 3.0
// due to the so-called vague linking.
//
// See also: http://gcc.gnu.org/faq.html#dso
// See also: http://www.codesourcery.com/public/cxx-abi/

class TRefCountedTracker
    : private TNonCopyable
{
public:
    static TRefCountedTracker* Get();

    TRefCountedTypeCookie GetCookie(
        TRefCountedTypeKey typeKey,
        const TSourceLocation& location = TSourceLocation());

    Y_FORCE_INLINE void Allocate(TRefCountedTypeCookie cookie, size_t size)
    {
        GetPerThreadSlot(cookie)->Allocate(size);
    }

    Y_FORCE_INLINE void Reallocate(TRefCountedTypeCookie cookie, size_t sizeFreed, size_t sizeAllocated)
    {
        GetPerThreadSlot(cookie)->Reallocate(sizeFreed, sizeAllocated);
    }

    Y_FORCE_INLINE void Free(TRefCountedTypeCookie cookie, size_t size)
    {
        GetPerThreadSlot(cookie)->Free(size);
    }

    TString GetDebugInfo(int sortByColumn = -1) const;
    NYson::TYsonProducer GetMonitoringProducer() const;

    i64 GetObjectsAllocated(TRefCountedTypeKey typeKey);
    i64 GetObjectsAlive(TRefCountedTypeKey typeKey);
    i64 GetAllocatedBytes(TRefCountedTypeKey typeKey);
    i64 GetAliveBytes(TRefCountedTypeKey typeKey);

    int GetTrackedThreadCount() const;

private:
    class TStatisticsHolder;
    friend class TStatisticsHolder;
    friend class TRefCountedTrackerInitializer;

    struct TKey
    {
        TRefCountedTypeKey TypeKey;
        TSourceLocation Location;

        bool operator < (const TKey& other) const;
        bool operator == (const TKey& other) const;
    };


    class TAnonymousSlot
    {
    public:
        Y_FORCE_INLINE void Allocate(i64 size)
        {
            ++ObjectsAllocated_;
            BytesAllocated_ += size;
        }

        Y_FORCE_INLINE void Reallocate(i64 sizeFreed, i64 sizeAllocated)
        {
            BytesFreed_ += sizeFreed;
            BytesAllocated_ += sizeAllocated;
        }

        Y_FORCE_INLINE void Free(i64 size)
        {
            ++ObjectsFreed_;
            BytesFreed_ += size;
        }

        TAnonymousSlot& operator += (const TAnonymousSlot& other);

        i64 GetObjectsAllocated() const;
        i64 GetObjectsAlive() const;
        i64 GetBytesAllocated() const;
        i64 GetBytesAlive() const;

    private:
        i64 ObjectsAllocated_ = 0;
        i64 BytesAllocated_ = 0;
        i64 ObjectsFreed_ = 0;
        i64 BytesFreed_ = 0;

    };

    typedef std::vector<TAnonymousSlot> TAnonymousStatistics;

    class TNamedSlot
        : public TAnonymousSlot
    {
    public:
        explicit TNamedSlot(const TKey& key);

        TRefCountedTypeKey GetTypeKey() const;
        const TSourceLocation& GetLocation() const;

        TString GetTypeName() const;
        TString GetFullName() const;

    private:
        TKey Key_;

    };

    typedef std::vector<TNamedSlot> TNamedStatistics;

    static PER_THREAD TAnonymousSlot* CurrentThreadStatisticsBegin;
    static PER_THREAD int CurrentThreadStatisticsSize;

    NConcurrency::TForkAwareSpinLock SpinLock_;
    std::map<TKey, TRefCountedTypeCookie> KeyToCookie_;
    std::vector<TKey> CookieToKey_;
    TAnonymousStatistics GlobalStatistics_;
    yhash_set<TStatisticsHolder*> PerThreadHolders_;


    TRefCountedTracker() = default;

    TNamedStatistics GetSnapshot() const;
    static void SortSnapshot(TNamedStatistics* snapshot, int sortByColumn);

    TNamedSlot GetSlot(TRefCountedTypeKey typeKey);

    Y_FORCE_INLINE TAnonymousSlot* GetPerThreadSlot(TRefCountedTypeCookie cookie)
    {
        if (cookie >= CurrentThreadStatisticsSize) {
            PreparePerThreadSlot(cookie);
        }
        return CurrentThreadStatisticsBegin + cookie;
    }

    void PreparePerThreadSlot(TRefCountedTypeCookie cookie);
    void FlushPerThreadStatistics(TStatisticsHolder* holder);

};

////////////////////////////////////////////////////////////////////////////////

//! A nifty counter initializer for TRefCountedTracker.
static class TRefCountedTrackerInitializer
{
public:
    TRefCountedTrackerInitializer();
} RefCountedTrackerInitializer;

// Never destroyed.
extern TRefCountedTracker* RefCountedTrackerInstance;

Y_FORCE_INLINE TRefCountedTracker* TRefCountedTracker::Get()
{
    return RefCountedTrackerInstance;
}

////////////////////////////////////////////////////////////////////////////////

// Typically invoked from GDB console.
void DumpRefCountedTracker(int sortByColumn = -1);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
