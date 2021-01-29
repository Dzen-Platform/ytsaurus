#pragma once

#include "private.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/core/logging/log.h>

#include <DataStreams/IBlockOutputStream.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

//! Creates CH wrapper which writes to static table.
DB::BlockOutputStreamPtr CreateStaticTableBlockOutputStream(
    NYPath::TRichYPath path,
    NTableClient::TTableSchemaPtr tableSchema,
    std::vector<DB::DataTypePtr> dataTypes,
    NTableClient::TTableWriterConfigPtr config,
    TCompositeSettingsPtr compositeSettings,
    NApi::NNative::IClientPtr client,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

//! Creates CH wrapper which writes to dynamic table.
DB::BlockOutputStreamPtr CreateDynamicTableBlockOutputStream(
    NYPath::TRichYPath path,
    NTableClient::TTableSchemaPtr tableSchema,
    std::vector<DB::DataTypePtr> dataTypes,
    TDynamicTableSettingsPtr settings,
    TCompositeSettingsPtr compositeSettings,
    NApi::NNative::IClientPtr client,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
