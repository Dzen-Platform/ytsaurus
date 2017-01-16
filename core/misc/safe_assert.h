#pragma once

#include <yt/ytlib/misc/public.h>

#include <yt/core/concurrency/public.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/nullable.h>
#include <yt/core/misc/property.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Thrown when an assertion is not satisfied and SafeAssertionMode = true.
class TAssertionFailedException
{
public:
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Expression);
    DEFINE_BYVAL_RO_PROPERTY(Stroka, StackTrace);
    DEFINE_BYVAL_RO_PROPERTY(TNullable<Stroka>, CorePath);

public:
    TAssertionFailedException(
        const Stroka& expression,
        const Stroka& stackTrace,
        const TNullable<Stroka>& corePath);
};

////////////////////////////////////////////////////////////////////////////////

class TSafeAssertionsGuard
{
public:
    TSafeAssertionsGuard() = default;
    TSafeAssertionsGuard(
        TCoreDumperPtr coreDumper,
        NConcurrency::TAsyncSemaphorePtr coreSemaphore);
    ~TSafeAssertionsGuard();

    TSafeAssertionsGuard(const TSafeAssertionsGuard& other) = delete;
    TSafeAssertionsGuard(TSafeAssertionsGuard&& other);

    TSafeAssertionsGuard& operator=(const TSafeAssertionsGuard& other) = delete;
    TSafeAssertionsGuard& operator=(TSafeAssertionsGuard&& other);

private:
    bool Active_ = false;

    void Release();
};

////////////////////////////////////////////////////////////////////////////////

void SetSafeAssertionsMode(
    TCoreDumperPtr coreDumper,
    NConcurrency::TAsyncSemaphorePtr coreSemaphore);

bool SafeAssertionsModeEnabled();

TCoreDumperPtr GetSafeAssertionsCoreDumper();

NConcurrency::TAsyncSemaphorePtr GetSafeAssertionsCoreSemaphore();

void ResetSafeAssertionsMode();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
