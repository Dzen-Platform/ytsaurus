#pragma once

#include "public.h"

#include <yt/core/concurrency/rw_spinlock.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

struct TMediumDescriptor
{
    Stroka Name;
    int Index = InvalidMediumIndex;
    int Priority = -1;

    bool operator == (const TMediumDescriptor& other) const;
    bool operator != (const TMediumDescriptor& other) const;
};

class TMediumDirectory
    : public TRefCounted
{
public:
    const TMediumDescriptor* FindByIndex(int index) const;
    const TMediumDescriptor* GetByIndexOrThrow(int index) const;

    const TMediumDescriptor* FindByName(const Stroka& name) const;
    const TMediumDescriptor* GetByNameOrThrow(const Stroka& name) const;

    void UpdateDirectory(const NProto::TMediumDirectory& protoDirectory);

private:
    mutable NConcurrency::TReaderWriterSpinLock SpinLock_;
    yhash<Stroka, const TMediumDescriptor*> NameToDescriptor_;
    yhash<int, const TMediumDescriptor*> IndexToDescriptor_;

    std::vector<std::unique_ptr<TMediumDescriptor>> Descriptors_;

};

DEFINE_REFCOUNTED_TYPE(TMediumDirectory)

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
