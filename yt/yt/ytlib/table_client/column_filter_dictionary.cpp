#include "column_filter_dictionary.h"

#include <yt/yt_proto/yt/client/table_chunk_format/proto/chunk_meta.pb.h>

#include <yt/yt/core/misc/protobuf_helpers.h>
#include <yt/yt/core/misc/collection_helpers.h>

#include <library/cpp/yt/farmhash/farm_hash.h>

#include <util/digest/multi.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

TColumnFilterDictionary::TColumnFilterDictionary(bool sortColumns)
    : SortColumns_(sortColumns)
{ }

int TColumnFilterDictionary::GetIdOrRegisterAdmittedColumns(std::vector<TString> admittedColumns)
{
    if (SortColumns_) {
        std::sort(admittedColumns.begin(), admittedColumns.end());
    }
    auto admittedColumnsIterator = AdmittedColumnsToId_.find(admittedColumns);
    if (admittedColumnsIterator == AdmittedColumnsToId_.end()) {
        int id = IdToAdmittedColumns_.size();
        IdToAdmittedColumns_.push_back(admittedColumns);
        admittedColumnsIterator = AdmittedColumnsToId_.emplace(admittedColumns, id).first;
    }
    return admittedColumnsIterator->second;
}

const std::vector<TString>& TColumnFilterDictionary::GetAdmittedColumns(int id) const
{
    return IdToAdmittedColumns_[id];
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TColumnFilterDictionary* protoDictionary, const TColumnFilterDictionary& dictionary)
{
    using NYT::ToProto;

    for (const auto& admittedColumns : dictionary.IdToAdmittedColumns_) {
        auto* protoColumnFilter = protoDictionary->add_column_filters();
        ToProto(protoColumnFilter->mutable_admitted_names(), admittedColumns);
    }
}

void FromProto(TColumnFilterDictionary* dictionary, const NProto::TColumnFilterDictionary& protoDictionary)
{
    using NYT::FromProto;

    for (const auto& columnFilter : protoDictionary.column_filters()) {
        dictionary->GetIdOrRegisterAdmittedColumns(FromProto<std::vector<TString>>(columnFilter.admitted_names()));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
