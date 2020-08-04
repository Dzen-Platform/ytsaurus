#pragma once

#include "private.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

// Truncate ytSubquery(<long base64-encoded stuff>) to make it human-readable.
TString MaybeTruncateSubquery(TString query);
TString SerializeAndMaybeTruncateSubquery(const DB::IAST& ast);

////////////////////////////////////////////////////////////////////////////////

} // namespace NClickHouseServer

////////////////////////////////////////////////////////////////////////////////

namespace NYson {

////////////////////////////////////////////////////////////////////////////////

template <class TAST>
void Serialize(const TAST& ast, NYson::IYsonConsumer* consumer, std::enable_if_t<std::is_convertible<TAST*, DB::IAST*>::value>* = nullptr);

template <class TAST>
void Serialize(const TAST* ast, NYson::IYsonConsumer* consumer, std::enable_if_t<std::is_convertible<TAST*, DB::IAST*>::value>* = nullptr);

template <class TAST>
void Serialize(const std::shared_ptr<TAST>& ast, NYson::IYsonConsumer* consumer, std::enable_if_t<std::is_convertible<TAST*, DB::IAST*>::value>* = nullptr);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYson

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define FORMAT_INL_H_
#include "format-inl.h"
#undef FFORMAT_INL_H_
