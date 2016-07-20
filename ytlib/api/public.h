#pragma once

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/misc/public.h>

namespace NYT {
namespace NApi {

///////////////////////////////////////////////////////////////////////////////

// Api error codes
DEFINE_ENUM(EErrorCode,
    ((TooManyConcurrentRequests)                         (1800))
);

///////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IRowset)

DECLARE_REFCOUNTED_STRUCT(IConnection)
DECLARE_REFCOUNTED_STRUCT(IAdmin)
DECLARE_REFCOUNTED_STRUCT(IClient)
DECLARE_REFCOUNTED_STRUCT(ITransaction)

DECLARE_REFCOUNTED_CLASS(TDispatcher)

DECLARE_REFCOUNTED_STRUCT(IFileReader)
DECLARE_REFCOUNTED_STRUCT(IFileWriter)

DECLARE_REFCOUNTED_STRUCT(IJournalReader)
DECLARE_REFCOUNTED_STRUCT(IJournalWriter)

DECLARE_REFCOUNTED_CLASS(TMasterConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TFileReaderConfig)
DECLARE_REFCOUNTED_CLASS(TFileWriterConfig)
DECLARE_REFCOUNTED_CLASS(TJournalReaderConfig)
DECLARE_REFCOUNTED_CLASS(TJournalWriterConfig)

///////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

