#include "secondary_query_header.h"

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

void TSerializableSpanContext::Register(TRegistrar registrar)
{
    registrar.BaseClassParameter("trace_id", &TSerializableSpanContext::TraceId);
    registrar.BaseClassParameter("span_id", &TSerializableSpanContext::SpanId);
    registrar.BaseClassParameter("sampled", &TSerializableSpanContext::Sampled);
}

////////////////////////////////////////////////////////////////////////////////

void TSecondaryQueryHeader::Register(TRegistrar registrar)
{
    registrar.Parameter("query_id", &TSecondaryQueryHeader::QueryId);
    registrar.Parameter("parent_query_id", &TSecondaryQueryHeader::ParentQueryId);
    registrar.Parameter("span_context", &TSecondaryQueryHeader::SpanContext);
    registrar.Parameter("transaction_id", &TThis::TransactionId);
    registrar.Parameter("storage_index", &TSecondaryQueryHeader::StorageIndex);
    registrar.Parameter("query_depth", &TSecondaryQueryHeader::QueryDepth);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
