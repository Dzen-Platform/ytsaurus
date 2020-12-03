#include "cube.h"

#include <yt/yt/library/profiling/summary.h>
#include <yt/yt/library/profiling/tag.h>

#include <yt/core/misc/assert.h>
#include <yt/core/misc/error.h>

#include <type_traits>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

template <class T>
bool TCube<T>::TProjection::IsZero(int index) const
{
    T zero{};
    return Values[index] == zero;
}

template <class T>
TCube<T>::TCube(int windowSize, i64 nextIteration)
    : WindowSize_(windowSize)
    , NextIteration_(nextIteration)
{ }

template <class T>
void TCube<T>::StartIteration()
{
    Index_ = GetIndex(NextIteration_);
    NextIteration_++;

    for (auto& [tagIds, projection] : Projections_) {
        projection.Rollup += projection.Values[Index_];
        projection.Values[Index_] = {};
    }
}

template <class T>
void TCube<T>::FinishIteration()
{ }

template <class T>
void TCube<T>::Add(const TTagIdList& tagIds)
{
    if (auto it = Projections_.find(tagIds); it != Projections_.end()) {
        it->second.UsageCount++;
    } else {
        TProjection projection;
        projection.UsageCount = 1;
        projection.Values.resize(WindowSize_);
        Projections_[tagIds] = std::move(projection);
    }
}

template <class T>
void TCube<T>::AddAll(const TTagIdList& tagIds, const TProjectionSet& projections)
{
    projections.Range(tagIds, [this] (auto tagIds) mutable {
        Add(tagIds);
    });
}

template <class T>
void TCube<T>::Remove(const TTagIdList& tagIds)
{
    auto it = Projections_.find(tagIds);
    if (it == Projections_.end()) {
        THROW_ERROR_EXCEPTION("Broken cube");
    }

    it->second.UsageCount--;
    if (it->second.UsageCount == 0) {
        Projections_.erase(it);
    }
}

template <class T>
void TCube<T>::RemoveAll(const TTagIdList& tagIds, const TProjectionSet& projections)
{
    projections.Range(tagIds, [this] (auto tagIds) mutable {
        Remove(tagIds);
    });
}

template <class T>
void TCube<T>::Update(const TTagIdList& tagIds, T value)
{
    auto it = Projections_.find(tagIds);
    if (it == Projections_.end()) {
        THROW_ERROR_EXCEPTION("Broken cube");
    }

    it->second.Values[Index_] += value;
    it->second.LastUpdateIteration = NextIteration_ - 1;
}

template <class T>
const THashMap<TTagIdList, typename TCube<T>::TProjection>& TCube<T>::GetProjections() const
{
    return Projections_;
}

template <class T>
int TCube<T>::GetSize() const
{
    return Projections_.size();
}

template <class T>
int TCube<T>::GetIndex(i64 iteration) const
{
    return iteration % WindowSize_;
}

template <class T>
T TCube<T>::Rollup(const TProjection& window, int index) const
{
    auto sum = window.Rollup;

    for (auto i = Index_ + 1; true; i++) {
        if (i == WindowSize_) {
            i = 0;
        }

        sum += window.Values[i];
        if (i == index) {
            break;
        }
    }

    return sum;
}

template <class T>
void TCube<T>::ReadSensors(
    const TString& name,
    const TReadOptions& options,
    const TTagRegistry& tagsRegistry,
    NMonitoring::IMetricConsumer* consumer) const
{
    auto writeLabels = [&] (const auto& tagIds, bool rate, bool max, bool allowAggregate) {
        consumer->OnLabelsBegin();

        auto sensorName = name + (rate ? "/rate" : "") + (max ? "/max" : "");
        for (auto& c : sensorName) {
            if (c == '/') {
                c = '.';
            }
        }

        consumer->OnLabel("sensor", sensorName);

        if (options.Global) {
            consumer->OnLabel("host", "");
        } else if (options.Host) {
            consumer->OnLabel("host", *options.Host);
        }

        SmallVector<bool, 8> replacedInstanceTags(options.InstanceTags.size());

        if (allowAggregate && options.MarkAggregates && !options.Global) {
            consumer->OnLabel("yt_aggr", "1");
        }

        for (auto tagId : tagIds) {
            const auto& tag = tagsRegistry.Decode(tagId);

            for (size_t i = 0; i < options.InstanceTags.size(); i++) {
                if (options.InstanceTags[i].first == tag.first) {
                    replacedInstanceTags[i] = true;
                }
            }

            consumer->OnLabel(tag.first, tag.second);
        }

        if (!options.Global) {
            for (size_t i = 0; i < options.InstanceTags.size(); i++) {
                if (replacedInstanceTags[i]) {
                    continue;
                }

                const auto& tag = options.InstanceTags[i];
                consumer->OnLabel(tag.first, tag.second);
            }
        }

        consumer->OnLabelsEnd();
    };

    auto skipByHack = [&] (const auto& window) {
        if (!options.Sparse) {
            return false;
        }

        for (const auto& readBatch : options.Times) {
            for (auto index : readBatch.first) {
                if (!window.IsZero(index)) {
                    return false;
                }
            }
        }

        return true;
    };

    auto skipSparse = [&] (auto window, const std::vector<int>& indices) {
        if (!options.Sparse) {
            return false;
        }

        for (auto index : indices) {
            if (!window.IsZero(index)) {
                return false;
            }
        }

        return true;
    };

    for (const auto& [tagIds, window] : Projections_) {
        if (options.EnableSolomonAggregationWorkaround && skipByHack(window)) {
            continue;
        }

        for (const auto& [indices, time] : options.Times) {
            if (!options.EnableSolomonAggregationWorkaround && skipSparse(window, indices)) {
                continue;
            }

            T value{};
            for (auto index : indices) {
                if (index < 0 || static_cast<size_t>(index) >= window.Values.size()) {
                    THROW_ERROR_EXCEPTION("Read index is invalid")
                        << TErrorAttribute("index", index)
                        << TErrorAttribute("window_size", window.Values.size());
                }

                value += window.Values[index];
            }

            if constexpr (std::is_same_v<T, i64>) {
                if (options.ConvertCountersToRateGauge) {
                    consumer->OnMetricBegin(NMonitoring::EMetricType::GAUGE);
                } else {
                    consumer->OnMetricBegin(NMonitoring::EMetricType::RATE);
                }

                writeLabels(tagIds, options.ConvertCountersToRateGauge, false, true);

                if (options.ConvertCountersToRateGauge) {
                    if (options.RateDenominator < 0.1) {
                        THROW_ERROR_EXCEPTION("Invalid rate denominator");
                    }

                    consumer->OnDouble(time, value / options.RateDenominator);
                } else {
                    consumer->OnInt64(time, Rollup(window, indices.back()));
                }
            } else if constexpr (std::is_same_v<T, double>) {
                consumer->OnMetricBegin(NMonitoring::EMetricType::GAUGE);

                writeLabels(tagIds, false, false, true);

                consumer->OnDouble(time, window.Values[indices.back()]);
            } else if constexpr (std::is_same_v<T, TSummarySnapshot<double>>) {
                if (options.ExportSummaryAsMax) {
                    consumer->OnMetricBegin(NMonitoring::EMetricType::GAUGE);
                } else {
                    consumer->OnMetricBegin(NMonitoring::EMetricType::DSUMMARY);
                }

                writeLabels(tagIds, false, options.ExportSummaryAsMax, !options.ExportSummaryAsMax);

                auto snapshot = MakeIntrusive<NMonitoring::TSummaryDoubleSnapshot>(
                    value.Sum(),
                    value.Min(),
                    value.Max(),
                    value.Last(),
                    static_cast<ui64>(value.Count())
                );
                if (options.ExportSummaryAsMax) {
                    consumer->OnDouble(time, snapshot->GetMax());
                } else {
                    consumer->OnSummaryDouble(time, snapshot);
                }
            } else if constexpr (std::is_same_v<T, TSummarySnapshot<TDuration>>) {
                if (options.ExportSummaryAsMax) {
                    consumer->OnMetricBegin(NMonitoring::EMetricType::GAUGE);
                } else {
                    consumer->OnMetricBegin(NMonitoring::EMetricType::DSUMMARY);
                }

                writeLabels(tagIds, false, options.ExportSummaryAsMax, !options.ExportSummaryAsMax);

                auto snapshot = MakeIntrusive<NMonitoring::TSummaryDoubleSnapshot>(
                    value.Sum().SecondsFloat(),
                    value.Min().SecondsFloat(),
                    value.Max().SecondsFloat(),
                    value.Last().SecondsFloat(),
                    static_cast<ui64>(value.Count())
                );

                if (options.ExportSummaryAsMax) {
                    consumer->OnDouble(time, snapshot->GetMax());
                } else {
                    consumer->OnSummaryDouble(time, snapshot);
                }
            } else {
                THROW_ERROR_EXCEPTION("Unexpected cube type");
            }

            consumer->OnMetricEnd();
        }
    }
}

template class TCube<double>;
template class TCube<i64>;
template class TCube<TSummarySnapshot<double>>;
template class TCube<TSummarySnapshot<TDuration>>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
