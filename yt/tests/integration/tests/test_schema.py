# -*- coding: utf8 -*-

from yt_env_setup import YTEnvSetup, unix_only
from yt_commands import *
import pytest

##################################################################

GOOD_VALUE_LIST = [
    {"i32": 2 ** 31 - 1}, {"i32": 0}, {"i32": - 2 ** 31}, {"i32": None},
    {"i16": 2 ** 15 - 1}, {"i16": 0}, {"i16": - 2 ** 15}, {"i16": None},
    {"i8": 2 ** 7 - 1}, {"i8": 0}, {"i8": - 2 ** 7}, {"i8": None},

    {"ui32": 0}, {"ui32": 2 ** 32 - 1}, {"ui32": None},
    {"ui16": 0}, {"ui16": 2 ** 16 - 1}, {"ui16": None},
    {"ui8": 0}, {"ui8": 2 ** 8 - 1}, {"ui8": None},

    {"utf8": "ff"}, {"utf8": "ЫТЬ"}, {"utf8": None},
]

BAD_VALUE_LIST = [
    {"i32": 2 ** 31}, {"i32": - 2 ** 31 - 1},
    {"i16": 2 ** 15}, {"i16": - 2 ** 15 - 1},
    {"i8": 2 ** 7}, {"i8": - 2 ** 7 - 1},

    {"ui32": 2 ** 32},
    {"ui16": 2 ** 16},
    {"ui8": 2 ** 8},

    {"utf8": "\xFF"},
]

SCHEMA = [
    {
        "type": "uint64",
        "name": "key",
        "sort_order": "ascending",
    },
    {
        "type": "uint32",
        "name": "ui32",
    },
    {
        "type": "uint16",
        "name": "ui16",
    },
    {
        "type": "uint8",
        "name": "ui8",
    },
    {
        "type": "int32",
        "name": "i32",
    },
    {
        "type": "int16",
        "name": "i16",
    },
    {
        "type": "int8",
        "name": "i8",
    },
    {
        "type": "utf8",
        "name": "utf8",
    },
]


class TestLogicalType(YTEnvSetup):
    USE_DYNAMIC_TABLES = True

    def test_static_tables(self):
        create("table", "//test-table", attributes={"schema": SCHEMA})

        # expect no assertions
        write_table("//test-table", GOOD_VALUE_LIST)

        for bad_value in BAD_VALUE_LIST:
            with pytest.raises(YtError):
                write_table("//test-table", [bad_value])

    def test_dynamic_tables(self):
        sync_create_cells(1)
        create("table", "//test-dynamic-table", attributes={"schema": SCHEMA, "dynamic": True})

        sync_mount_table("//test-dynamic-table")

        key = 0
        for val in GOOD_VALUE_LIST:
            key += 1
            val = val.copy()
            val["key"] = key
            insert_rows("//test-dynamic-table", [val])

        for val in BAD_VALUE_LIST:
            key += 1
            val = val.copy()
            val["key"] = key
            with pytest.raises(YtError):
                insert_rows("//test-dynamic-table", [val])

        sync_unmount_table("//test-dynamic-table")

    def test_bad_alter_table(self):
        def single_column_schema(typename):
            return [{"name": "column_name", "type": typename}]

        def expect_error_alter_table(schema_before, schema_after):
            remove("//test-alter-table", force=True)
            create("table", "//test-alter-table", attributes={"schema": schema_before})
            # Make table nonempty, since empty table allows any alter
            write_table("//test-alter-table", [{}])
            with pytest.raises(YtResponseError):
                alter_table("//test-alter-table", schema=schema_after)

        for (source_type, bad_destination_type_list) in [
            ("int8", ["uint64", "uint8", "string"]),
            ("int16", ["uint16", "uint16", "string", "int8"]),
            ("int32", ["uint32", "uint32", "string", "int8", "int16"]),
        ]:
            for destination_type in bad_destination_type_list:
                expect_error_alter_table(
                    [{"name": "column_name", "type": source_type}],
                    [{"name": "column_name", "type": destination_type}])

    def test_logical_type_column_constrains(self):
        with pytest.raises(YtError):
            create("table", "//test-table",
                   attributes={"schema": [
                       {"name": "key1", "type": "int32", "expression": "100"},
                   ]})

        with pytest.raises(YtError):
            create("table", "//test-table",
                   attributes={"schema": [
                       {"name": "key1", "type": "int32", "aggregate": "sum"},
                   ]})

class TestRequiredOption(YTEnvSetup):
    USE_DYNAMIC_TABLES = True
    NUM_SCHEDULERS = 1
    def test_required_static_tables(self):
        create("table", "//tmp/required_table",
               attributes={
                   "schema": [
                       {
                           "name": "value",
                           "type": "string",
                           "required": True,
                       }
                   ],
               })

        write_table("//tmp/required_table", [{"value": "foo"}])
        with pytest.raises(YtError):
            write_table("//tmp/required_table", [{"value": 100500}])
        with pytest.raises(YtError):
            write_table("//tmp/required_table", [{"value": None}])
        with pytest.raises(YtError):
            write_table("//tmp/required_table", [{}])

    def test_required_any_is_disallowed(self):
        with pytest.raises(YtError):
            create("table", "//tmp/required_table",
                   attributes={
                       "schema": [
                           {
                               "name": "value",
                               "type": "any",
                               "required": True,
                           }
                       ],
                   })
        with pytest.raises(YtError):
            create("table", "//tmp/dynamic_required_table",
                   attributes={
                       "dynamic": True,
                       "schema": [
                           {
                               "name": "key",
                               "type": "string",
                               "sort_order": "ascending",
                           },
                           {
                               "name": "value",
                               "type": "any",
                               "required": True,
                           }
                       ],
                   })

    def test_alter_required_column(self):
        table = "//tmp/static_table"
        create("table", table,
               attributes={
                   "schema": [
                       {
                           "name": "column",
                           "type": "string",
                       }
                   ],
               })
        write_table(table, [{"column": None}])
        with pytest.raises(YtError):
            alter_table(
                table,
                schema=[
                    {
                        "name": "column",
                        "type": "string",
                        "required": True,
                    }
                ]
            )
        write_table(table, [{"column": None}])

        create("table", table,
               force=True,
               attributes={
                   "schema": [
                       {
                           "name": "column",
                           "type": "string",
                           "required": True,
                       }
                   ],
               })
        write_table(table, [{"column": "foo"}])

        # No exception.
        alter_table(
            table,
            schema=[
                {
                    "name": "column",
                    "type": "string",
                }
            ]
        )

    @pytest.mark.parametrize("sorted_table", [False, True])
    def test_infer_required_column(self, sorted_table):
        if sorted_table:
            schema = make_schema([
                {"name": "key", "type": "string", "required": False, "sort_order": "ascending"},
                {"name": "value", "type": "string", "required": True},
            ], unique_keys=False, strict=True)
        else:
            schema = make_schema([
                {"name": "key", "type": "string", "required": False},
                {"name": "value", "type": "string", "required": True},
            ], unique_keys=False, strict=True)
        table = "//tmp/input1"
        create("table", table, attributes={"schema": schema})
        table = "//tmp/input2"
        create("table", table, attributes={"schema": schema})
        write_table("//tmp/input1", [{"key": "foo", "value": "bar"}])
        write_table("//tmp/input2", [{"key": "foo", "value": "baz"}])

        create("table", "//tmp/output")

        mode = "sorted" if sorted_table else "unordered"
        merge(in_=["//tmp/input1", "//tmp/input2"], out="//tmp/output", mode=mode)
        assert get("//tmp/output/@schema") == schema

    def test_infer_mixed_requiredness(self):
        table = "//tmp/input1"
        create("table", table, attributes={"schema": make_schema([
            {"name": "value", "type": "string", "required": True},
        ], unique_keys=False, strict=True)})
        table = "//tmp/input2"
        create("table", table, attributes={"schema": make_schema([
            {"name": "value", "type": "string", "required": False},
        ], unique_keys=False, strict=True)})

        write_table("//tmp/input1", [{"value": "foo"}])
        write_table("//tmp/input2", [{"value": "bar"}])

        create("table", "//tmp/output")

        with pytest.raises(YtError):
            # Schemas are incompatible
            merge(in_=["//tmp/input1", "//tmp/input2"], out="//tmp/output", mode="unordered")

    def test_required_columns_in_dynamic_tables_schema(self):
        schema = [
                {"name": "key_req", "type": "int64", "sort_order": "ascending", "required": True},
                {"name": "key_opt", "type": "int64", "sort_order": "ascending"},
                {"name": "value_req", "type": "string", "required": True},
                {"name": "value_opt", "type": "string"}]

        sync_create_cells(1)
        create("table", "//tmp/t", attributes={"schema": schema, "dynamic": True})

        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", [{"key_req": 1, "key_opt": 2, "value_req": "x", "value_opt": "y"}])
        sync_unmount_table("//tmp/t")

        # Required columns cannot be added
        with pytest.raises(YtError):
            alter_table("//tmp/t", schema=schema + [{"name": "value3_req", "type": "string", "required": True}])

        # Adding non-required columns is OK
        schema += [{"name": "value3_opt", "type": "string", "required": False}]
        alter_table("//tmp/t", schema=schema)

        # Old column cannot become required
        bad_schema = [i.copy() for i in schema]
        bad_schema[3]["required"] = True
        with pytest.raises(YtError):
            alter_table("//tmp/t", schema=bad_schema)

        # Removing 'required' attribute is OK
        good_schema = [i.copy() for i in schema]
        good_schema[2]["required"] = False
        alter_table("//tmp/t", schema=good_schema)

class TestSchemaDeduplication(YTEnvSetup):
    def test_empty_schema(self):
        create("table", "//tmp/table")
        assert get("//tmp/table/@schema_duplicate_count") == 0

    def test_simple_schema(self):
        def get_schema(strict):
            return make_schema([{"name": "value", "type": "string", "required": True}], unique_keys=False, strict=strict)

        create("table", "//tmp/table1", attributes={"schema": get_schema(True)})
        create("table", "//tmp/table2", attributes={"schema": get_schema(True)})
        create("table", "//tmp/table3", attributes={"schema": get_schema(False)})

        assert get("//tmp/table1/@schema_duplicate_count") == 2
        assert get("//tmp/table2/@schema_duplicate_count") == 2
        assert get("//tmp/table3/@schema_duplicate_count") == 1

        alter_table("//tmp/table2", schema=get_schema(False))

        assert get("//tmp/table1/@schema_duplicate_count") == 1
        assert get("//tmp/table2/@schema_duplicate_count") == 2
        assert get("//tmp/table3/@schema_duplicate_count") == 2


class TestSchemaValidation(YTEnvSetup):
    def test_schema_complexity(self):
        def make_schema(size):
            return [
                {"name": "column{}".format(i), "type": "int64"}
                for i in range(size)
            ]
        def make_row(size):
            return {"column{}".format(i) : i for i in range(size)}

        bad_size = 32 * 1024
        with pytest.raises(YtError):
            create("table", "//tmp/bad-schema-1", attributes={"schema": make_schema(bad_size)})

        create("table", "//tmp/bad-schema-2")
        with pytest.raises(YtError):
            write_table("//tmp/bad-schema-2", [make_row(bad_size)])

        ok_size = bad_size - 1
        create("table", "//tmp/ok-schema", attributes={"schema": make_schema(ok_size)})
        write_table("//tmp/ok-schema", [make_row(ok_size)])
