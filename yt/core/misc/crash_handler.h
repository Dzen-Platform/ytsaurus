#pragma once

#include <yt/core/misc/nullable.h>

#include <util/generic/stroka.h>

#include <set>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

void InstallCrashSignalHandler(TNullable<std::set<int>> signalNumbers = Null);

////////////////////////////////////////////////////////////////////////////////

// "Codicils" are short human- and machine-readable strings organized into a per-fiber stack.
// When the crash handler is invoked, it dumps (alongside with the other
// useful stuff like backtrace) the content of the latter stack.

//! Installs a new codicil into the stack.
void PushCodicil(const Stroka& data);
//! Removes the top codicils from the stack.
void PopCodicil();

//! Invokes #PushCodicil in ctor and #PopCodicil in dtor.
class TCodicilGuard
{
public:
    TCodicilGuard();
    explicit TCodicilGuard(const Stroka& data);
    ~TCodicilGuard();

    TCodicilGuard(const TCodicilGuard& other) = delete;
    TCodicilGuard(TCodicilGuard&& other);

    TCodicilGuard& operator=(const TCodicilGuard& other) = delete;
    TCodicilGuard& operator=(TCodicilGuard&& other);


private:
    bool Active_;

    void Release();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
