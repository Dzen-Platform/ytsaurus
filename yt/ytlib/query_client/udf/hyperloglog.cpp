#include <yt/ytlib/new_table_client/unversioned_value.cpp>
#include <contrib/libs/farmhash/farmhash.cc>
#include <core/misc/hyperloglog.h>
#include <yt_udf_cpp.h>

static uint64_t Hash(TUnversionedValue* v)
{
    auto value = (NYT::NVersionedTableClient::TUnversionedValue*)v;
    return NYT::NVersionedTableClient::GetFarmFingerprint(*value);
}

typedef NYT::THyperLogLog<14> THLL;

extern "C" void cardinality_init(
    TExecutionContext* context,
    TUnversionedValue* result)
{
    auto hll = AllocatePermanentBytes(context, sizeof(THLL));
    new (hll) THLL();

    result->Type = EValueType::String;
    result->Length = sizeof(THLL);
    result->Data.String = hll;
}

extern "C" void cardinality_update(
    TExecutionContext* context,
    TUnversionedValue* result,
    TUnversionedValue* state,
    TUnversionedValue* newValue)
{
    result->Type = EValueType::String;
    result->Length = sizeof(THLL);
    result->Data.String = state->Data.String;

    auto hll = reinterpret_cast<THLL*>(state->Data.String);
    hll->Add(Hash(newValue));
}

extern "C" void cardinality_merge(
    TExecutionContext* context,
    TUnversionedValue* result,
    TUnversionedValue* state1,
    TUnversionedValue* state2)
{
    result->Type = EValueType::String;
    result->Length = sizeof(THLL);
    result->Data.String = state1->Data.String;

    auto hll1 = reinterpret_cast<THLL*>(state1->Data.String);
    auto hll2 = reinterpret_cast<THLL*>(state2->Data.String);
    hll1->Merge(*hll2);
}

extern "C" void cardinality_finalize(
    TExecutionContext* context,
    TUnversionedValue* result,
    TUnversionedValue* state)
{
    auto hll = reinterpret_cast<THLL*>(state->Data.String);
    result->Type = EValueType::Uint64;
    result->Data.Uint64 = hll->EstimateCardinality();
}
