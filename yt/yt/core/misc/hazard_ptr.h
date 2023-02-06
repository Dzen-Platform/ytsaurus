#pragma once

#include <yt/yt/core/misc/public.h>

#include <library/cpp/yt/logging/logger.h>

#include <atomic>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger LockFreePtrLogger;

bool ScanDeleteList();
void FlushDeleteList();

using THazardPtrDeleter = void(*)(void*);

void ScheduleObjectDeletion(void* ptr, THazardPtrDeleter deleter);

////////////////////////////////////////////////////////////////////////////////

struct THazardPtrFlushGuard
{
    THazardPtrFlushGuard();
    ~THazardPtrFlushGuard();
};

////////////////////////////////////////////////////////////////////////////////

//! Protects an object from destruction (or deallocation) before CAS.
//! Destruction or deallocation depends on delete callback in ScheduleObjectDeletion.
template <class T>
class THazardPtr
{
public:
    static_assert(T::EnableHazard, "T::EnableHazard must be true.");

    THazardPtr() = default;
    THazardPtr(const THazardPtr&) = delete;
    THazardPtr(THazardPtr&& other);

    THazardPtr& operator=(const THazardPtr&) = delete;
    THazardPtr& operator=(THazardPtr&& other);

    template <class TPtrLoader>
    static THazardPtr Acquire(TPtrLoader&& ptrLoader, T* ptr);
    template <class TPtrLoader>
    static THazardPtr Acquire(TPtrLoader&& ptrLoader);

    void Reset();

    ~THazardPtr();

    T* Get() const;

    // Operators * and -> are allowed to use only when hazard ptr protects from object
    // destruction (ref count decrementation). Not memory deallocation.
    T& operator*() const;
    T* operator->() const;

    explicit operator bool() const;

private:
    THazardPtr(T* ptr, std::atomic<void*>* hazardPtr);

    T* Ptr_ = nullptr;
    std::atomic<void*>* HazardPtr_ = nullptr;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define HAZARD_PTR_INL_H_
#include "hazard_ptr-inl.h"
#undef HAZARD_PTR_INL_H_
