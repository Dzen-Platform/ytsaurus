#include "helpers.h"

#include "schema.h"
#include "table.h"
#include "config.h"

#include <yt/ytlib/api/native/client.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/client/table_client/unversioned_row.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/ytree/permission.h>
#include <yt/core/ytree/fluent.h>

#include <yt/core/logging/log.h>

#include <Common/FieldVisitors.h>

#include <DataTypes/DataTypeNullable.h>

#include <Interpreters/ExpressionActions.h>
#include <Interpreters/ProcessList.h>

#include <Storages/MergeTree/KeyCondition.h>

#include <Access/AccessControlManager.h>
#include <Access/User.h>

#include <util/string/escape.h>

namespace NYT::NClickHouseServer {

using namespace NTableClient;
using namespace NYPath;
using namespace NYTree;
using namespace NLogging;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NChunkClient;
using namespace NApi;
using namespace NConcurrency;
using namespace NYson;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

TGuid ToGuid(DB::UUID uuid)
{
    TGuid result;
    memcpy(&result, &uuid, sizeof(uuid));
    return result;
}

////////////////////////////////////////////////////////////////////////////////

void RegisterNewUser(DB::AccessControlManager& accessControlManager, TString userName)
{
    auto user = std::make_unique<DB::User>();
    user->setName(userName);
    user->access.grant(DB::AccessFlags::allFlags(), "YT" /* database */);
    user->access.grant(DB::AccessFlags::allFlags(), "system" /* database */);
    user->access.grant(DB::AccessType::CREATE_TEMPORARY_TABLE);
    user->access.grant(DB::AccessType::dictGet);

    accessControlManager.tryInsert(std::move(user));
}

////////////////////////////////////////////////////////////////////////////////

std::optional<DB::Field> TryGetMinimumTypeValue(const DB::DataTypePtr& dataType)
{
    switch (dataType->getTypeId()) {
        case DB::TypeIndex::Nullable:
            return DB::Field();
        case DB::TypeIndex::Int64:
            return DB::Field(std::numeric_limits<DB::Int64>::min());
        case DB::TypeIndex::UInt64:
            return DB::Field(std::numeric_limits<DB::UInt64>::min());
        case DB::TypeIndex::Float64:
            return DB::Field(-std::numeric_limits<DB::Float64>::infinity());
        case DB::TypeIndex::String:
            return DB::Field("");
        default:
            THROW_ERROR_EXCEPTION("Unexpected data type %v", dataType->getName());
    }
}

std::optional<DB::Field> TryGetMaximumTypeValue(const DB::DataTypePtr& dataType)
{
    switch (dataType->getTypeId()) {
        case DB::TypeIndex::Nullable:
            return TryGetMaximumTypeValue(DB::removeNullable(dataType));
        case DB::TypeIndex::Int64:
            return DB::Field(std::numeric_limits<DB::Int64>::max());
        case DB::TypeIndex::UInt64:
            return DB::Field(std::numeric_limits<DB::UInt64>::max());
        case DB::TypeIndex::Float64:
            return DB::Field(std::numeric_limits<DB::Float64>::infinity());
        case DB::TypeIndex::String:
            return std::nullopt;
        default:
            THROW_ERROR_EXCEPTION("Unexpected data type %v", dataType->getName());
    }
}

std::optional<DB::Field> TryDecrementFieldValue(const DB::Field& field, const DB::DataTypePtr& dataType)
{
    if (auto minValue = TryGetMinimumTypeValue(dataType); !minValue || *minValue == field) {
        return std::nullopt;
    }
    switch (dataType->getTypeId()) {
        case DB::TypeIndex::Nullable:
            // When the decremented value is unrepresented in removeNullable(dataType),
            // we theoreticly can represent it as Null, because Null is smaller than any value.
            // But we do not care since this function declared to help only in 'simple cases'.
            return TryDecrementFieldValue(field, DB::removeNullable(dataType));
        case DB::TypeIndex::Int64:
            return DB::Field(field.get<Int64>() - 1);
        case DB::TypeIndex::UInt64:
            return DB::Field(field.get<UInt64>() - 1);
        case DB::TypeIndex::Float64:
            // Not supported yet.
            return std::nullopt;
        case DB::TypeIndex::String:
            // Not supported yet.
            return std::nullopt;
        default:
            THROW_ERROR_EXCEPTION("Unexpected data type %v", dataType->getName());
    }
}

////////////////////////////////////////////////////////////////////////////////

DB::Field ConvertToField(const NTableClient::TUnversionedValue& value)
{
    switch (value.Type) {
        case EValueType::Null:
            return DB::Field();
        case EValueType::Int64:
            return DB::Field(static_cast<DB::Int64>(value.Data.Int64));
        case EValueType::Uint64:
            return DB::Field(static_cast<DB::UInt64>(value.Data.Uint64));
        case EValueType::Double:
            return DB::Field(static_cast<DB::Float64>(value.Data.Double));
        case EValueType::Boolean:
            return DB::Field(static_cast<DB::UInt64>(value.Data.Boolean ? 1 : 0));
        case EValueType::String:
        case EValueType::Any:
        case EValueType::Composite:
            return DB::Field(value.Data.String, value.Length);
        default:
            THROW_ERROR_EXCEPTION("Unexpected data type %Qlv", value.Type);
    }
}

void ConvertToUnversionedValue(const DB::Field& field, TUnversionedValue* value)
{
    switch (value->Type) {
        case EValueType::Int64:
        case EValueType::Uint64:
        case EValueType::Double: {
            memcpy(&value->Data, &field.reinterpret<ui64>(), sizeof(value->Data));
            break;
        }
        case EValueType::Boolean: {
            if (field.get<ui64>() > 1) {
                THROW_ERROR_EXCEPTION("Cannot convert value %v to boolean", field.get<ui64>());
            }
            memcpy(&value->Data, &field.get<ui64>(), sizeof(value->Data));
            break;
        }
        case EValueType::String: {
            const auto& str = field.get<std::string>();
            value->Data.String = str.data();
            value->Length = str.size();
            break;
        }
        default: {
            THROW_ERROR_EXCEPTION("Unexpected data type %Qlv", value->Type);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

TQuerySettingsPtr ParseCustomSettings(
    const TQuerySettingsPtr baseSettings,
    const DB::Settings::Range& customSettings,
    const TLogger& logger)
{
    const auto& Logger = logger;

    auto result = New<TQuerySettings>();
    auto node = ConvertToNode(baseSettings);
    for (const auto& setting : customSettings) {
        auto settingName = TString(setting.getName());
        YT_VERIFY(settingName.StartsWith("chyt"));
        if (!settingName.StartsWith("chyt.") && !settingName.StartsWith("chyt_")) {
            THROW_ERROR_EXCEPTION(
                "Invalid setting name %Qv; CHYT settings should start with \"chyt.\" or with \"chyt_\" prefix",
                settingName);
        }
        TYPath ypath = "/" + settingName.substr(5);
        for (auto& character : ypath) {
            if (character == '.') {
                character = '/';
            }
        }
        auto field = setting.getValue();
        auto fieldType = ToValueType(field.getType());
        YT_LOG_TRACE("Parsing custom setting (YPath: %v, FieldValue: %v)", ypath, field.dump());
        
        auto modifiedNode = FindNodeByYPath(node, ypath);

        INodePtr patchNode;
        if (modifiedNode && fieldType == EValueType::String && modifiedNode->GetType() != ENodeType::String) {
            // If we expect something diffrent from string, then try to convert it.
            const auto& stringVal = field.get<std::string>();
            patchNode = ConvertToNode(TYsonStringBuf(stringVal));
        } else {
            TUnversionedValue unversionedValue;
            unversionedValue.Id = 0;
            unversionedValue.Type = fieldType;
            ConvertToUnversionedValue(field, &unversionedValue);
            patchNode = ConvertToNode(unversionedValue);
        }

        YT_LOG_TRACE("Patch node (Node: %v)", ConvertToYsonString(patchNode, EYsonFormat::Text));
        SetNodeByYPath(node, ypath, patchNode);
    }

    YT_LOG_TRACE("Resulting node (Node: %v)", ConvertToYsonString(node, EYsonFormat::Text));
    result->SetUnrecognizedStrategy(EUnrecognizedStrategy::KeepRecursive);
    result->Load(node);

    YT_LOG_DEBUG(
        "Custom settings parsed (Settings: %v, Unrecognized: %v)",
        ConvertToYsonString(result, EYsonFormat::Text),
        ConvertToYsonString(result->GetUnrecognizedRecursively(), EYsonFormat::Text));

    return result;
}

////////////////////////////////////////////////////////////////////////////////

TTableSchemaPtr InferCommonSchema(const std::vector<TTablePtr>& tables, const TLogger& logger)
{
    THashSet<TTableSchema> schemas;
    for (const auto& table : tables) {
        schemas.emplace(*table->Schema);
    }

    if (schemas.empty()) {
        return New<TTableSchema>();
    }

    if (schemas.size() == 1) {
        return New<TTableSchema>(*schemas.begin());
    }

    const auto& Logger = logger;

    const auto& firstSchema = schemas.begin();

    THashMap<TString, TColumnSchema> nameToColumn;
    THashMap<TString, size_t> nameCounter;

    for (const auto& column : firstSchema->Columns()) {
        auto [it, _] = nameToColumn.emplace(column.Name(), column);
        // We will set sorted order for key columns later.
        it->second.SetSortOrder(std::nullopt);
    }

    for (const auto& schema : schemas) {
        for (const auto& column : schema.Columns()) {
            if (auto it = nameToColumn.find(column.Name()); it != nameToColumn.end()) {
                if (it->second.CastToV1Type() == column.CastToV1Type()) {
                    ++nameCounter[column.Name()];
                    if (!column.Required() && it->second.Required()) {
                        // If at least in one schema the column isn't required, the result common column isn't required too.
                        it->second.SetLogicalType(OptionalLogicalType(it->second.LogicalType()));
                    }
                }
            }
        }
    }

    std::vector<TColumnSchema> resultColumns;
    resultColumns.reserve(firstSchema->Columns().size());
    for (const auto& column : firstSchema->Columns()) {
        if (nameCounter[column.Name()] == schemas.size()) {
            resultColumns.push_back(nameToColumn[column.Name()]);
        }
    }

    for (size_t index = 0; index < resultColumns.size(); ++index) {
        bool isKeyColumn = true;
        for (const auto& schema : schemas) {
            if (schema.Columns().size() <= index) {
                isKeyColumn = false;
                break;
            }
            const auto& column = schema.Columns()[index];
            if (column.Name() != resultColumns[index].Name() || !column.SortOrder()) {
                isKeyColumn = false;
                break;
            }
        }
        if (!isKeyColumn) {
            // Key columns are the prefix of all columns, so all following collumns aren't key.
            break;
        }
        resultColumns[index].SetSortOrder(ESortOrder::Ascending);
    }

    auto commonSchema = New<TTableSchema>(std::move(resultColumns));

    YT_LOG_INFO("Common schema inferred (Schemas: %v, CommonSchema: %v)",
        schemas,
        *commonSchema);

    return commonSchema;
}

////////////////////////////////////////////////////////////////////////////////

//! Leaves only some of the "significant" profile counters.
THashMap<TString, size_t> GetBriefProfileCounters(const ProfileEvents::Counters& profileCounters)
{
    static const std::vector<ProfileEvents::Event> SignificantEvents = {
        ProfileEvents::Query,
        ProfileEvents::SelectQuery,
        ProfileEvents::InsertQuery,
        ProfileEvents::InsertedRows,
        ProfileEvents::InsertedBytes,
        ProfileEvents::ContextLock,
        ProfileEvents::RealTimeMicroseconds,
        ProfileEvents::UserTimeMicroseconds,
        ProfileEvents::SystemTimeMicroseconds,
        ProfileEvents::SoftPageFaults,
        ProfileEvents::HardPageFaults,
        ProfileEvents::OSIOWaitMicroseconds,
        ProfileEvents::OSCPUWaitMicroseconds,
        ProfileEvents::OSCPUVirtualTimeMicroseconds,
        ProfileEvents::OSReadChars,
        ProfileEvents::OSWriteChars,
        ProfileEvents::OSReadBytes,
        ProfileEvents::OSWriteBytes,
    };

    THashMap<TString, size_t> result;

    for (const auto& event : SignificantEvents) {
        result[CamelCaseToUnderscoreCase(ProfileEvents::getName(event))] = profileCounters[event].load(std::memory_order::memory_order_relaxed);
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer

namespace DB {

////////////////////////////////////////////////////////////////////////////////

TString ToString(const NameSet& nameSet)
{
    return NYT::Format("%v", std::vector<TString>(nameSet.begin(), nameSet.end()));
}

void Serialize(const QueryStatusInfo& query, NYT::NYson::IYsonConsumer* consumer)
{
    NYT::NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("query").Value(NYT::NClickHouseServer::MaybeTruncateSubquery(TString(query.query)))
            .Item("elapsed_seconds").Value(query.elapsed_seconds)
            .Item("read_rows").Value(query.read_rows)
            .Item("read_bytes").Value(query.read_bytes)
            .Item("total_rows").Value(query.total_rows)
            .Item("written_rows").Value(query.written_rows)
            .Item("written_bytes").Value(query.written_bytes)
            .Item("memory_usage").Value(query.memory_usage)
            .Item("peak_memory_usage").Value(query.peak_memory_usage)
        .EndMap();
}

void Serialize(const ProcessListForUserInfo& processListForUserInfo, NYT::NYson::IYsonConsumer* consumer)
{
    NYT::NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("memory_usage").Value(processListForUserInfo.memory_usage)
            .Item("peak_memory_usage").Value(processListForUserInfo.peak_memory_usage)
            .Item("brief_profile_counters").Value(NYT::NClickHouseServer::GetBriefProfileCounters(*processListForUserInfo.profile_counters))
        .EndMap();
}

TString ToString(const Field& field)
{
    return EscapeC(TString(field.dump()));
}

TString ToString(const Block& block)
{
    NYT::TStringBuilder content;
    const auto& columns = block.getColumns();
    content.AppendChar('{');
    for (size_t rowIndex = 0; rowIndex < block.rows(); ++rowIndex) {
        if (rowIndex != 0) {
            content.AppendString(", ");
        }
        content.AppendChar('{');
        for (size_t columnIndex = 0; columnIndex < block.columns(); ++columnIndex) {
            if (columnIndex != 0) {
                content.AppendString(", ");
            }
            const auto& field = (*columns[columnIndex])[rowIndex];
            content.AppendString(applyVisitor(FieldVisitorToString(), field));
        }
        content.AppendChar('}');
    }
    content.AppendChar('}');

    return NYT::Format(
        "{RowCount: %v, ColumnCount: %v, Structure: {%v}, Content: %v}",
        block.rows(),
        block.columns(),
        block.dumpStructure(),
        content.Flush());
}

void PrintTo(const Field& field, std::ostream* os)
{
    *os << ToString(field);
}


////////////////////////////////////////////////////////////////////////////////

} // namespace DB
