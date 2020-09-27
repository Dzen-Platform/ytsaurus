#pragma once

#include <yt/core/misc/common.h>

namespace NYT::NFormats {

////////////////////////////////////////////////////////////////////////////////

constexpr ui32 LenvalTableIndexMarker = static_cast<ui32>(-1);
constexpr ui32 LenvalKeySwitch = static_cast<ui32>(-2);
constexpr ui32 LenvalRangeIndexMarker = static_cast<ui32>(-3);
constexpr ui32 LenvalRowIndexMarker = static_cast<ui32>(-4);
constexpr ui32 LenvalEndOfStream = static_cast<ui32>(-5);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFormats
