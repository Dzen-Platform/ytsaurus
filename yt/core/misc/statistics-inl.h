#pragma once
#ifndef STATISTICS_INL_H_
#error "Direct inclusion of this file is not allowed, include statistics.h"
// For the sake of sane code completion.
#include "statistics.h"
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class T>
void TStatistics::AddSample(const NYPath::TYPath& path, const T& sample)
{
    AddSample(path, NYTree::ConvertToNode(sample));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
