#pragma once

#include "public.h"

#include <yt/core/misc/string.h>

#include <util/generic/typetraits.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

struct TVersion
{
    int SegmentId;
    int RecordId;

    TVersion() noexcept;
    TVersion(int segmentId, int recordId) noexcept;

    bool operator < (TVersion other) const;
    bool operator == (TVersion other) const;
    bool operator != (TVersion other) const;
    bool operator > (TVersion other) const;
    bool operator <= (TVersion other) const;
    bool operator >= (TVersion other) const;

    ui64 ToRevision() const;
    static TVersion FromRevision(ui64 revision);

    TVersion Advance(int delta = 1) const;
    TVersion Rotate() const;
};

void FormatValue(TStringBuilderBase* builder, TVersion version, TStringBuf spec);
TString ToString(TVersion version);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra

Y_DECLARE_PODTYPE(NYT::NHydra::TVersion);
