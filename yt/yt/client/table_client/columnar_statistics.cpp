#include "columnar_statistics.h"

#include <numeric>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

TColumnarStatistics& TColumnarStatistics::operator +=(const TColumnarStatistics& other)
{
    if (other.ColumnDataWeights.empty()) {
        return *this;
    }

    for (int index = 0; index < ColumnDataWeights.size(); ++index) {
        ColumnDataWeights[index] += other.ColumnDataWeights[index];
    }
    if (other.TimestampTotalWeight) {
        TimestampTotalWeight = TimestampTotalWeight.value_or(0) + *other.TimestampTotalWeight;
    }
    LegacyChunkDataWeight += other.LegacyChunkDataWeight;
    return *this;
}

TColumnarStatistics TColumnarStatistics::MakeEmpty(int columnCount)
{
    return TColumnarStatistics{std::vector<i64>(columnCount, 0), std::nullopt, 0};
}

TLightweightColumnarStatistics TColumnarStatistics::MakeLightweightStatistics() const
{
    return TLightweightColumnarStatistics{
        .ColumnDataWeightsSum = std::accumulate(ColumnDataWeights.begin(), ColumnDataWeights.end(), (i64)0),
        .TimestampTotalWeight = TimestampTotalWeight,
        .LegacyChunkDataWeight = LegacyChunkDataWeight
    };
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
