#pragma once
#ifndef CHUNK_VISITOR_INL_H
#error "Direct inclusion of this file is not allowed, include chunk_visitor.h"
#endif

#include <yt/core/ytree/helpers.h>
#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

template <class TKeyExtractor>
class TChunkStatisticsVisitor
    : public TChunkVisitorBase
{
public:
    TChunkStatisticsVisitor(
        NCellMaster::TBootstrap* bootstrap,
        TChunkList* chunkList,
        TKeyExtractor keyExtractor)
        : TChunkVisitorBase(bootstrap, chunkList)
        , KeyExtractor_(keyExtractor)
    { }

private:
    const TKeyExtractor KeyExtractor_;

    using TKey = typename std::result_of<TKeyExtractor(const TChunk*)>::type;
    using TStatiticsMap = yhash<TKey, TChunkTreeStatistics>;
    TStatiticsMap StatisticsMap_;

    virtual bool OnChunk(
        TChunk* chunk,
        i64 /*rowIndex*/,
        const NChunkClient::TReadLimit& /*startLimit*/,
        const NChunkClient::TReadLimit& /*endLimit*/) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        StatisticsMap_[KeyExtractor_(chunk)].Accumulate(chunk->GetStatistics());
        return true;
    }

    virtual void OnSuccess() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto result = NYTree::BuildYsonStringFluently()
            .DoMapFor(StatisticsMap_, [=] (NYTree::TFluentMap fluent, const typename TStatiticsMap::value_type& pair) {
                const auto& statistics = pair.second;
                // TODO(panin): maybe use here the same method as in attributes
                fluent
                    .Item(FormatKey(pair.first)).BeginMap()
                        .Item("chunk_count").Value(statistics.ChunkCount)
                        .Item("uncompressed_data_size").Value(statistics.UncompressedDataSize)
                        .Item("compressed_data_size").Value(statistics.CompressedDataSize)
                    .EndMap();
            });
        Promise_.Set(result);
    }

    template <class T>
    static TString FormatKey(T value, typename TEnumTraits<T>::TType* = 0)
    {
        return FormatEnum(value);
    }

    static TString FormatKey(NObjectClient::TCellTag value)
    {
        return ToString(value);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TKeyExtractor>
TFuture<NYson::TYsonString> ComputeChunkStatistics(
    NCellMaster::TBootstrap* bootstrap,
    TChunkList* chunkList,
    TKeyExtractor keyExtractor)
{
    auto visitor = New<TChunkStatisticsVisitor<TKeyExtractor>>(
        bootstrap,
        chunkList,
        keyExtractor);
    return visitor->Run();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT

