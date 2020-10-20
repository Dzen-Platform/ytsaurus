#pragma once

#include "private.h"

#include "format.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/client/table_client/schema.h>

#include <yt/client/ypath/public.h>

#include <yt/core/logging/public.h>

#include <Core/Field.h>
#include <Core/Block.h>
#include <Core/Settings.h>
#include <Storages/ColumnsDescription.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

TGuid ToGuid(DB::UUID uuid);

void RegisterNewUser(DB::AccessControlManager& accessControlManager, TString userName);

////////////////////////////////////////////////////////////////////////////////

DB::Field ConvertToField(const NTableClient::TUnversionedValue& value);

// TODO(dakovalkov): Working with elements as DB::Field is inefficient. It copies complex values (strings/arrays, etc.).
// Try to not use this function.
// TODO(dakovalkov): Remove this function.
//! `value` should have Type field filled.
void ConvertToUnversionedValue(const DB::Field& field, NTableClient::TUnversionedValue* value);

////////////////////////////////////////////////////////////////////////////////

TQuerySettingsPtr ParseCustomSettings(
    TQuerySettingsPtr baseSettings,
    const DB::Settings::Range& customSettings,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

// Returns the schema with all common columns.
// If the column is missed in any tables or the type of the column mismatch in different schemas, the column will be ommited.
// If at least in one schema the column doesn't have "required" flag, the column will be not required.
// Key columns are maximum prefix of key collumns in all schemas.
NTableClient::TTableSchemaPtr InferCommonSchema(const std::vector<TTablePtr>& tables, const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

//! Leaves only some of the "significant" profile counters.
THashMap<TString, size_t> GetBriefProfileCounters(const ProfileEvents::Counters& profileCounters);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer

namespace DB {

////////////////////////////////////////////////////////////////////////////////

void Serialize(const QueryStatusInfo& queryStatusInfo, NYT::NYson::IYsonConsumer* consumer);
void Serialize(const ProcessListForUserInfo& processListForUserInfo, NYT::NYson::IYsonConsumer* consumer);

TString ToString(const Field& field);

TString ToString(const Block& block);

void PrintTo(const Field& field, ::std::ostream* os);

////////////////////////////////////////////////////////////////////////////////

} // namespace DB
