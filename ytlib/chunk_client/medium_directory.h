#pragma once

#include "public.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/core/concurrency/rw_spinlock.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

struct TMediumDescriptor
{
    TString Name;
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

    const TMediumDescriptor* FindByName(const TString& name) const;
    const TMediumDescriptor* GetByNameOrThrow(const TString& name) const;

    std::vector<int> GetMediumIndexes() const;

    void LoadFrom(const NProto::TMediumDirectory& protoDirectory);

    void Clear();

private:
    mutable NConcurrency::TReaderWriterSpinLock SpinLock_;
    THashMap<TString, const TMediumDescriptor*> NameToDescriptor_;
    THashMap<int, const TMediumDescriptor*> IndexToDescriptor_;

    std::vector<std::unique_ptr<TMediumDescriptor>> Descriptors_;

};

DEFINE_REFCOUNTED_TYPE(TMediumDirectory)

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TMediumDirectoryPtr& mediumDirectory, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
