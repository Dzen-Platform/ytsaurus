from yt_commands import *
from yt.test_helpers import assert_items_equal

from yt_env_setup import YTEnvSetup, unix_only

import protobuf_format


def assert_rowsets_equal(first, second):
    assert len(first) == len(second)
    for i, (row_first, row_second) in enumerate(zip(first, second)):
        assert row_first == row_second, "Mismatch at index {}".format(i)


def create_protobuf_format(configs, enumerations):
    format = yson.YsonString("protobuf")
    format.attributes["tables"] = configs
    format.attributes["enumerations"] = ENUMERATIONS
    return format


ENUMERATIONS = {
    "MyEnum": {
        "Red": 12,
        "Yellow": 2,
        "Green": -42,
    },
}

SCHEMALESS_TABLE_ROWS = [
    {
        "int64_column": -42,
        "uint64_column": yson.YsonUint64(25),
        "double_column": 3.14,
        "bool_column": True,
        "string_column": "foo",
        "any_column": [110, "xxx", {"foo": "bar"}],
        "enum_string_column": "Red",
        "enum_int_column": 12,
    },
    {
        "int64_column": -15,
        "uint64_column": yson.YsonUint64(25),
        "double_column": 2.7,
        "bool_column": False,
        "string_column": "qux",
        "any_column": 234,
        "enum_string_column": "Green",
        "enum_int_column": -42,
    },
]

PROTOBUF_SCHEMALESS_TABLE_ROWS = SCHEMALESS_TABLE_ROWS

SCHEMALESS_TABLE_PROTOBUF_CONFIG = {
    "columns": [
        {
            "name": "int64_column",
            "field_number": 1,
            "proto_type": "int64",
        },
        {
            "name": "uint64_column",
            "field_number": 2,
            "proto_type": "uint64",
        },
        {
            "name": "double_column",
            "field_number": 3,
            "proto_type": "double",
        },
        {
            "name": "bool_column",
            "field_number": 4,
            "proto_type": "bool",
        },
        {
            "name": "string_column",
            "field_number": 5,
            "proto_type": "string",
        },
        {
            "name": "any_column",
            "field_number": 6,
            "proto_type": "any",
        },
        {
            "name": "enum_string_column",
            "field_number": 7,
            "enumeration_name": "MyEnum",
            "proto_type": "enum_string",
        },
        {
            "name": "enum_int_column",
            "field_number": 8,
            "enumeration_name": "MyEnum",
            "proto_type": "enum_int",
        },
    ],
}


class TestSchemalessProtobufFormat(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    @authors("levysotsky")
    def test_protobuf_read(self):
        create("table", "//tmp/t")
        write_table("//tmp/t", SCHEMALESS_TABLE_ROWS)
        table_config = SCHEMALESS_TABLE_PROTOBUF_CONFIG
        format = create_protobuf_format([table_config], ENUMERATIONS)
        data = read_table("//tmp/t", output_format=format)
        parsed_rows = protobuf_format.parse_lenval_protobuf(data, format)
        assert_rowsets_equal(parsed_rows, PROTOBUF_SCHEMALESS_TABLE_ROWS)

    @authors("levysotsky")
    def test_protobuf_write(self):
        create("table", "//tmp/t")
        table_config = SCHEMALESS_TABLE_PROTOBUF_CONFIG
        format = create_protobuf_format([table_config], ENUMERATIONS)
        data = protobuf_format.write_lenval_protobuf(SCHEMALESS_TABLE_ROWS, format)
        assert_rowsets_equal(
            protobuf_format.parse_lenval_protobuf(data, format),
            PROTOBUF_SCHEMALESS_TABLE_ROWS
        )
        write_table("//tmp/t", value=data, is_raw=True, input_format=format)
        read_rows = read_table("//tmp/t")
        assert_rowsets_equal(read_rows, SCHEMALESS_TABLE_ROWS)

    @authors("levysotsky")
    @unix_only
    def test_multi_output_map(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", SCHEMALESS_TABLE_ROWS)

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        table_config = SCHEMALESS_TABLE_PROTOBUF_CONFIG
        input_format = create_protobuf_format([table_config], ENUMERATIONS)
        output_format = create_protobuf_format([table_config] * 2, ENUMERATIONS)

        protobuf_dump = read_table("//tmp/t_in", output_format=input_format)
        parsed_rows = protobuf_format.parse_lenval_protobuf(protobuf_dump, input_format)
        assert_rowsets_equal(parsed_rows, PROTOBUF_SCHEMALESS_TABLE_ROWS)

        map(
            in_="//tmp/t_in",
            out=["//tmp/t_out1", "//tmp/t_out2"],
            command="tee /dev/fd/4",
            spec={
                "mapper": {
                    "input_format": input_format,
                    "output_format": output_format,
                },
                "job_count": 1,
            },
        )

        assert_rowsets_equal(read_table("//tmp/t_out1"), SCHEMALESS_TABLE_ROWS)
        assert_rowsets_equal(read_table("//tmp/t_out2"), SCHEMALESS_TABLE_ROWS)

    @authors("levysotsky")
    @unix_only
    def test_multi_output_map_wrong_config(self):
        """Check bad format (number of tables in format doesn't match number of output tables)"""
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", SCHEMALESS_TABLE_ROWS)

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        table_config = SCHEMALESS_TABLE_PROTOBUF_CONFIG
        input_format = create_protobuf_format([table_config], ENUMERATIONS)
        # Note single table config.
        output_format = create_protobuf_format([table_config], ENUMERATIONS)

        try:
            map(
                in_="//tmp/t_in",
                out=["//tmp/t_out1", "//tmp/t_out2"],
                command="tee /dev/fd/4",
                spec={
                    "mapper": {
                        "input_format": input_format,
                        "output_format": output_format,
                    },
                    "max_failed_job_count": 1,
                },
            )
            assert False, "Mapper should have failed"
        except YtError as error:
            assert "Protobuf format does not have table with index 1" in str(error)

    @authors("levysotsky")
    @unix_only
    def test_id_map(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", SCHEMALESS_TABLE_ROWS)

        create("table", "//tmp/t_out")

        table_config = SCHEMALESS_TABLE_PROTOBUF_CONFIG
        format = create_protobuf_format([table_config], ENUMERATIONS)

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat",
            spec={
                "mapper": {
                    "format": format,
                },
                "job_count": 1,
            },
        )

        protobuf_dump = read_table("//tmp/t_in", output_format=format)
        parsed_rows = protobuf_format.parse_lenval_protobuf(protobuf_dump, format)
        assert_rowsets_equal(parsed_rows, PROTOBUF_SCHEMALESS_TABLE_ROWS)

        assert_rowsets_equal(read_table("//tmp/t_out"), SCHEMALESS_TABLE_ROWS)

        format = create_protobuf_format(
            [
                {
                    "columns": [
                        {
                            "name": "other_columns",
                            "field_number": 1,
                            "proto_type": "other_columns",
                        },
                    ],
                },
            ],
            ENUMERATIONS,
        )

        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat",
            spec={
                "mapper": {
                    "format": format,
                },
                "job_count": 1,
            },
        )

        assert_rowsets_equal(read_table("//tmp/t_out"), SCHEMALESS_TABLE_ROWS)

    @authors("levysotsky")
    @unix_only
    def test_id_map_reduce(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", SCHEMALESS_TABLE_ROWS)

        create("table", "//tmp/t_out")

        table_config = SCHEMALESS_TABLE_PROTOBUF_CONFIG
        format = create_protobuf_format([table_config], ENUMERATIONS)

        map_reduce(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            reduce_by=["int64_column"],
            sort_by=["int64_column"],
            spec={
                "mapper": {
                    "format": format,
                    "command": "cat",
                },
                "reducer": {
                    "format": format,
                    "command": "cat",
                },
            },
        )

        protobuf_dump = read_table("//tmp/t_in", output_format=format)
        parsed_rows = protobuf_format.parse_lenval_protobuf(protobuf_dump, format)
        assert_rowsets_equal(parsed_rows, PROTOBUF_SCHEMALESS_TABLE_ROWS)
        assert_items_equal(read_table("//tmp/t_out"), SCHEMALESS_TABLE_ROWS)


SCHEMA = [
    {
        "name": "int16",
        "type_v3": "int16",
    },
    {
        "name": "list_of_strings",
        "type_v3": list_type("string"),
    },
    {
        "name": "optional_boolean",
        "type_v3": optional_type("bool"),
    },
    {
        "name": "list_of_optional_any",
        "type_v3": list_type(optional_type("yson")),
    },
    {
        "name": "struct",
        "type_v3": struct_type([
            ("key", "string"),
            ("points", list_type(
                struct_type([
                    ("x", "int64"),
                    ("y", "int64"),
                ])
            )),
            ("enum_string", "string"),
            ("enum_int", "int32"),
            ("extra_field", optional_type("string")),
        ]),
    },
    {
        "name": "utf8",
        "type_v3": "utf8",
    },
]

SCHEMAFUL_TABLE_PROTOBUF_CONFIG = {
    "columns": [
        {
            "name": "int16",
            "field_number": 1,
            "proto_type": "int32",
        },
        {
            "name": "list_of_strings",
            "field_number": 2122,
            "proto_type": "string",
            "repeated": True,
        },
        {
            "name": "optional_boolean",
            "field_number": 3,
            "proto_type": "bool",
        },
        {
            "name": "list_of_optional_any",
            "field_number": 4,
            "proto_type": "any",
            "repeated": True,
        },
        # Note that "extra_field" is missing from protobuf description.
        {
            "name": "struct",
            "field_number": 5,
            "proto_type": "structured_message",
            "fields": [
                {
                    "name": "key",
                    "field_number": 117,
                    "proto_type": "string",
                },
                {
                    "name": "points",
                    "field_number": 1,
                    "proto_type": "structured_message",
                    "repeated": True,
                    "fields": [
                        {
                            "name": "x",
                            "field_number": 1,
                            "proto_type": "int64",
                        },
                        {
                            "name": "y",
                            "field_number": 2,
                            "proto_type": "int64",
                        },
                    ],
                },
                {
                    "name": "enum_int",
                    "field_number": 2,
                    "enumeration_name": "MyEnum",
                    "proto_type": "enum_int",
                },
                {
                    "name": "enum_string",
                    "field_number": 3,
                    "enumeration_name": "MyEnum",
                    "proto_type": "enum_string",
                },
            ]
        },
        {
            "name": "utf8",
            "field_number": 6,
            "proto_type": "string",
        },
    ],
}

HELLO_WORLD = b"\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, \xd0\xbc\xd0\xb8\xd1\x80!"
GOODBYE_WORLD = b"\xd0\x9f\xd0\xbe\xd0\xba\xd0\xb0, \xd0\xbc\xd0\xb8\xd1\x80!"

SCHEMAFUL_TABLE_ROWS = [
    {
        "int16": 32767,
        "list_of_strings": ["foo", "bar", "baz"],
        "optional_boolean": False,
        "list_of_optional_any": [yson.YsonEntity(), {"x": 3}, []],
        "struct": {
            "key": "qux",
            "points": [{"x": 1, "y": 4}, {"x": 5, "y": 4}],
            "enum_int": -42,
            "enum_string": "Green",
            "extra_field": "baz",
        },
        "utf8": HELLO_WORLD,
    },
    {
        "int16": -32768,
        "list_of_strings": ["a", "bc"],
        "optional_boolean": None,
        "list_of_optional_any": [[yson.YsonEntity()], [1, "baz"]],
        "struct": {
            "key": "lol",
            "points": [],
            "enum_int": 12,
            "enum_string": "Red",
        },
        "utf8": GOODBYE_WORLD,
    },
]

PROTOBUF_SCHEMAFUL_TABLE_ROWS = [
    {
        "int16": 32767,
        "list_of_strings": ["foo", "bar", "baz"],
        "optional_boolean": False,
        "list_of_optional_any": [yson.YsonEntity(), {"x": 3}, []],
        "struct": {
            "key": "qux",
            "points": [{"x": 1, "y": 4}, {"x": 5, "y": 4}],
            "enum_int": -42,
            "enum_string": "Green",
        },
        "utf8": HELLO_WORLD,
    },
    {
        "int16": -32768,
        "list_of_strings": ["a", "bc"],
        "list_of_optional_any": [[yson.YsonEntity()], [1, "baz"]],
        "struct": {
            "key": "lol",
            "enum_int": 12,
            "enum_string": "Red",
        },
        "utf8": GOODBYE_WORLD,
    },
]

SCHEMAFUL_TABLE_ROWS_WITH_ENTITY_EXTRA_FIELD = [
    {
        "int16": 32767,
        "list_of_strings": ["foo", "bar", "baz"],
        "optional_boolean": False,
        "list_of_optional_any": [yson.YsonEntity(), {"x": 3}, []],
        "struct": {
            "key": "qux",
            "points": [{"x": 1, "y": 4}, {"x": 5, "y": 4}],
            "enum_int": -42,
            "enum_string": "Green",
            "extra_field": yson.YsonEntity(),
        },
        "utf8": HELLO_WORLD,
    },
    {
        "int16": -32768,
        "list_of_strings": ["a", "bc"],
        "optional_boolean": None,
        "list_of_optional_any": [[yson.YsonEntity()], [1, "baz"]],
        "struct": {
            "key": "lol",
            "points": [],
            "enum_int": 12,
            "enum_string": "Red",
            "extra_field": yson.YsonEntity(),
        },
        "utf8": GOODBYE_WORLD,
    },
]


@authors("levysotsky")
class TestSchemafulProtobufFormat(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    @authors("levysotsky")
    def test_protobuf_read(self):
        create("table", "//tmp/t", attributes={"schema": SCHEMA})
        write_table("//tmp/t", SCHEMAFUL_TABLE_ROWS)
        table_config = SCHEMAFUL_TABLE_PROTOBUF_CONFIG
        format = create_protobuf_format([table_config], ENUMERATIONS)
        data = read_table("//tmp/t", output_format=format)
        parsed_rows = protobuf_format.parse_lenval_protobuf(data, format)
        assert_rowsets_equal(parsed_rows, PROTOBUF_SCHEMAFUL_TABLE_ROWS)

    @authors("levysotsky")
    def test_protobuf_write(self):
        create("table", "//tmp/t", attributes={"schema": SCHEMA})
        table_config = SCHEMAFUL_TABLE_PROTOBUF_CONFIG
        format = create_protobuf_format([table_config], ENUMERATIONS)
        data = protobuf_format.write_lenval_protobuf(PROTOBUF_SCHEMAFUL_TABLE_ROWS, format)
        assert_rowsets_equal(
            protobuf_format.parse_lenval_protobuf(data, format),
            PROTOBUF_SCHEMAFUL_TABLE_ROWS
        )
        write_table("//tmp/t", value=data, is_raw=True, input_format=format)
        assert_rowsets_equal(
            read_table("//tmp/t"),
            SCHEMAFUL_TABLE_ROWS_WITH_ENTITY_EXTRA_FIELD
        )

    @unix_only
    def test_multi_output_map(self):
        create("table", "//tmp/t_in", attributes={"schema": SCHEMA})
        write_table("//tmp/t_in", SCHEMAFUL_TABLE_ROWS)

        table_config = SCHEMAFUL_TABLE_PROTOBUF_CONFIG
        input_format = create_protobuf_format([table_config], ENUMERATIONS)
        output_format = create_protobuf_format([table_config] * 2, ENUMERATIONS)

        protobuf_dump = read_table("//tmp/t_in", output_format=input_format)
        parsed_rows = protobuf_format.parse_lenval_protobuf(protobuf_dump, input_format)
        assert_rowsets_equal(parsed_rows, PROTOBUF_SCHEMAFUL_TABLE_ROWS)

        create("table", "//tmp/t_out1", attributes={"schema": SCHEMA})
        create("table", "//tmp/t_out2", attributes={"schema": SCHEMA})

        map(
            in_="//tmp/t_in",
            out=["//tmp/t_out1", "//tmp/t_out2"],
            command="tee /dev/fd/4",
            spec={
                "mapper": {
                    "input_format": input_format,
                    "output_format": output_format,
                },
                "job_count": 1,
            },
        )

        assert_rowsets_equal(read_table("//tmp/t_out1"), SCHEMAFUL_TABLE_ROWS_WITH_ENTITY_EXTRA_FIELD)
        assert_rowsets_equal(read_table("//tmp/t_out2"), SCHEMAFUL_TABLE_ROWS_WITH_ENTITY_EXTRA_FIELD)
