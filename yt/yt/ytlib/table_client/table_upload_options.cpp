#include "table_upload_options.h"
#include "helpers.h"

#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/ytree/helpers.h>

namespace NYT::NTableClient {

using namespace NChunkClient;
using namespace NCompression;
using namespace NCypressClient;
using namespace NYPath;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TTableSchemaPtr TTableUploadOptions::GetUploadSchema() const
{
    switch (SchemaModification) {
        case ETableSchemaModification::None:
            return TableSchema;

        case ETableSchemaModification::UnversionedUpdate:
            return TableSchema->ToUnversionedUpdate(/*sorted*/ true);

        default:
            YT_ABORT();
    }
}

void TTableUploadOptions::Persist(const NPhoenix::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, UpdateMode);
    Persist(context, LockMode);
    Persist<TNonNullableIntrusivePtrSerializer<>>(context, TableSchema);
    Persist(context, SchemaModification);
    Persist(context, SchemaMode);
    Persist(context, OptimizeFor);
    Persist(context, CompressionCodec);
    Persist(context, ErasureCodec);
    Persist(context, SecurityTags);
    Persist(context, PartiallySorted);
}

////////////////////////////////////////////////////////////////////////////////

static void ValidateSortColumnsEqual(const TSortColumns& sortColumns, const TTableSchema& schema)
{
    if (sortColumns != schema.GetSortColumns()) {
        THROW_ERROR_EXCEPTION("YPath attribute \"sorted_by\" must be compatible with table schema for a \"strong\" schema mode")
            << TErrorAttribute("sort_columns", sortColumns)
            << TErrorAttribute("table_schema", schema);
    }
}

static void ValidateAppendKeyColumns(const TSortColumns& sortColumns, const TTableSchema& schema, i64 rowCount)
{
    ValidateSortColumns(sortColumns);

    if (rowCount == 0) {
        return;
    }

    auto tableSortColumns = schema.GetSortColumns();
    bool areKeyColumnsCompatible = true;
    if (tableSortColumns.size() < sortColumns.size()) {
        areKeyColumnsCompatible = false;
    } else {
        for (int i = 0; i < std::ssize(sortColumns); ++i) {
            if (tableSortColumns[i] != sortColumns[i]) {
                areKeyColumnsCompatible = false;
                break;
            }
        }
    }

    if (!areKeyColumnsCompatible) {
        THROW_ERROR_EXCEPTION("Sort columns mismatch while trying to append sorted data into a non-empty table")
            << TErrorAttribute("append_sort_columns", sortColumns)
            << TErrorAttribute("table_sort_columns", tableSortColumns);
    }
}

TTableUploadOptions GetTableUploadOptions(
    const TRichYPath& path,
    const IAttributeDictionary& cypressTableAttributes,
    const TTableSchemaPtr& schema,
    i64 rowCount)
{
    auto schemaMode = cypressTableAttributes.Get<ETableSchemaMode>("schema_mode");
    auto optimizeFor = cypressTableAttributes.Get<EOptimizeFor>("optimize_for", EOptimizeFor::Lookup);
    auto compressionCodec = cypressTableAttributes.Get<NCompression::ECodec>("compression_codec");
    auto erasureCodec = cypressTableAttributes.Get<NErasure::ECodec>("erasure_codec", NErasure::ECodec::None);
    auto dynamic = cypressTableAttributes.Get<bool>("dynamic");

    // Some ypath attributes are not compatible with attribute "schema".
    if (path.GetAppend() && path.GetSchema()) {
        THROW_ERROR_EXCEPTION("YPath attributes \"append\" and \"schema\" are not compatible")
            << TErrorAttribute("path", path);
    }

    if (!path.GetSortedBy().empty() && path.GetSchema()) {
        THROW_ERROR_EXCEPTION("YPath attributes \"sorted_by\" and \"schema\" are not compatible")
            << TErrorAttribute("path", path);
    }

    // Dynamic tables have their own requirements as well.
    if (dynamic) {
        if (path.GetSchema()) {
            THROW_ERROR_EXCEPTION("YPath attribute \"schema\" cannot be set on a dynamic table")
                << TErrorAttribute("path", path);
        }

        if (!path.GetSortedBy().empty()) {
            THROW_ERROR_EXCEPTION("YPath attribute \"sorted_by\" cannot be set on a dynamic table")
                << TErrorAttribute("path", path);
        }
    }

    TTableUploadOptions result;
    auto pathSchema = path.GetSchema();
    if (path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        ValidateSortColumnsEqual(path.GetSortedBy(), *schema);

        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        // Old behaviour.
        ValidateAppendKeyColumns(path.GetSortedBy(), *schema, rowCount);

        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Weak;
        result.TableSchema = TTableSchema::FromSortColumns(path.GetSortedBy());
    } else if (path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        result.LockMode = (schema->IsSorted() && !dynamic) ? ELockMode::Exclusive : ELockMode::Shared;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        // Old behaviour - reset key columns if there were any.
        result.LockMode = ELockMode::Shared;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Weak;
        result.TableSchema = New<TTableSchema>();
    } else if (!path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        ValidateSortColumnsEqual(path.GetSortedBy(), *schema);

        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (!path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Weak;
        result.TableSchema = TTableSchema::FromSortColumns(path.GetSortedBy());
    } else if (!path.GetAppend() && pathSchema && (schemaMode == ETableSchemaMode::Strong)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = pathSchema;
    } else if (!path.GetAppend() && pathSchema && (schemaMode == ETableSchemaMode::Weak)) {
        // Change from Weak to Strong schema mode.
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = pathSchema;
    } else if (!path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (!path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Weak;
    } else {
        // Do not use YT_ABORT here, since this code is executed inside scheduler.
        THROW_ERROR_EXCEPTION("Failed to define upload parameters")
            << TErrorAttribute("path", path)
            << TErrorAttribute("schema_mode", schemaMode)
            << TErrorAttribute("schema", *schema);
    }

    if (path.GetAppend() && path.GetOptimizeFor()) {
        THROW_ERROR_EXCEPTION("YPath attributes \"append\" and \"optimize_for\" are not compatible")
            << TErrorAttribute("path", path);
    }

    if (path.GetOptimizeFor()) {
        result.OptimizeFor = *path.GetOptimizeFor();
    } else {
        result.OptimizeFor = optimizeFor;
    }

    if (path.GetAppend() && path.GetCompressionCodec()) {
        THROW_ERROR_EXCEPTION("YPath attributes \"append\" and \"compression_codec\" are not compatible")
            << TErrorAttribute("path", path);
    }

    if (path.GetCompressionCodec()) {
        result.CompressionCodec = *path.GetCompressionCodec();
    } else {
        result.CompressionCodec = compressionCodec;
    }

    if (path.GetAppend() && path.GetErasureCodec()) {
        THROW_ERROR_EXCEPTION("YPath attributes \"append\" and \"erasure_codec\" are not compatible")
            << TErrorAttribute("path", path);
    }

    result.ErasureCodec = path.GetErasureCodec().value_or(erasureCodec);

    if (path.GetSchemaModification() == ETableSchemaModification::UnversionedUpdateUnsorted) {
        THROW_ERROR_EXCEPTION("YPath attribute \"schema_modification\" cannot have value %Qlv for output tables",
            path.GetSchemaModification())
            << TErrorAttribute("path", path);
    } else if (!dynamic && path.GetSchemaModification() != ETableSchemaModification::None) {
        THROW_ERROR_EXCEPTION("YPath attribute \"schema_modification\" can have value %Qlv only for dynamic tables",
            path.GetSchemaModification())
            << TErrorAttribute("path", path);
    }
    result.SchemaModification = path.GetSchemaModification();

    if (!dynamic && path.GetPartiallySorted()) {
        THROW_ERROR_EXCEPTION("YPath attribute \"partially_sorted\" can be set only for dynamic tables")
            << TErrorAttribute("path", path);
    }
    result.PartiallySorted = path.GetPartiallySorted();

    result.SecurityTags = path.GetSecurityTags();

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
