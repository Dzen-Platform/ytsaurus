from yt.testlib import authors

from yt.wrapper.schema import (
    yt_dataclass,
    TableSchema,
    OutputRow,
    Int8,
    Int16,
    Int32,
    Int64,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
    YsonBytes,
    OtherColumns,
)

from yt.wrapper.prepare_operation import TypedJob

import yt.yson as yson
import yt.wrapper as yt

import yandex.type_info.typing as ti

import copy
import pytest
import typing


@yt_dataclass
class Struct:
    str_field: str
    uint8_field: typing.Optional[Uint64]


@yt_dataclass
class TheRow:
    int32_field: Int64
    str_field: str
    struct_field: typing.Optional[Struct] = None


@yt_dataclass
class RowWithContext(TheRow):
    row_index: int = -1
    table_index: int = -1


@yt_dataclass
class Row1:
    str_field: str
    int32_field: Int64


@yt_dataclass
class SumRow:
    key: str
    sum: Int64


ROW_DICTS = [
    {
        "int32_field": -(2 ** 31),
        "str_field": "Привет, мир",
        "struct_field": {
            "str_field": "И тебе привет",
            "uint8_field": yson.YsonUint64(255),
        },
    },
    {
        "int32_field": -123,
        "str_field": "spam",
        "struct_field": {
            "str_field": "foobar",
            "uint8_field": yson.YsonUint64(123),
        },
    },
]

ROWS = [
    TheRow(
        int32_field=-(2 ** 31),
        str_field="Привет, мир",
        struct_field=Struct(
            str_field="И тебе привет",
            uint8_field=255,
        ),
    ),
    TheRow(
        int32_field=-123,
        str_field="spam",
        struct_field=Struct(
            str_field="foobar",
            uint8_field=123,
        ),
    ),
]


@yt.with_context
class IdentityMapper(TypedJob):
    def __init__(self, row_types=None):
        if row_types is None:
            row_types = [TheRow]
        self._row_types = row_types

    def prepare_operation(self, context, preparer):
        for i, row_type in enumerate(self._row_types):
            preparer.input(i, type=row_type).output(i, type=row_type)

    def __call__(self, row, context):
        yield OutputRow(row, table_index=context.get_table_index())

    def get_intermediate_stream_count(self):
        return len(self._row_types)


@yt.with_context
class MapperWithContext(TypedJob):
    def prepare_operation(self, context, preparer):
        preparer.inputs([0, 1], type=TheRow).output(0, type=RowWithContext)

    def __call__(self, row, context):
        yield RowWithContext(
            int32_field=row.int32_field,
            str_field=row.str_field,
            struct_field=row.struct_field,
            row_index=context.get_row_index(),
            table_index=context.get_table_index(),
        )


class IdentityReducer(TypedJob):
    def prepare_operation(self, context, preparer):
        preparer.input(0, type=TheRow).output(0, type=TheRow)

    def __call__(self, rows):
        for row in rows:
            yield row


class EmptyInputReducer(TypedJob):
    def prepare_operation(self, context, preparer):
        preparer.input(0, type=TheRow).output(0, type=TheRow)

    def __call__(self, rows):
        assert False, "The method should have never been called"

    def finish(self):
        # Check that the job has actually been run.
        yield ROWS[0]


@yt.with_context
class TwoOutputMapper(TypedJob):
    def __init__(self, for_map_reduce):
        self._for_map_reduce = for_map_reduce

    def prepare_operation(self, context, preparer):
        (preparer.input(0, type=TheRow).output(0, type=TheRow).output(1, type=Row1))

    def __call__(self, row, context):
        assert context.get_table_index() == 0
        yield OutputRow(row, table_index=0)
        yield OutputRow(
            Row1(int32_field=row.int32_field + 1, str_field="Mapped: " + row.str_field),
            table_index=1,
        )

    def get_intermediate_stream_count(self):
        return 2 if self._for_map_reduce else 0


@yt.aggregator
class AggregatorTwoOutputMapper(TypedJob):
    def prepare_operation(self, context, preparer):
        (preparer.input(0, type=TheRow).output(0, type=TheRow).output(1, type=Row1))

    def __call__(self, rows):
        for row, context in rows.with_context():
            assert context.get_table_index() == 0
            yield OutputRow(row, table_index=0)
            yield OutputRow(
                Row1(int32_field=row.int32_field + 1, str_field="Mapped: " + row.str_field),
                table_index=1,
            )


@yt.with_context
class MapperWithBigPrepareOperation(TypedJob):
    def __init__(self, first_output_schema):
        self._first_output_schema = first_output_schema

    def prepare_operation(self, context, preparer):
        (
            preparer.input(
                0,
                type=TheRow,
                column_filter=["int32_field", "str_field", "struct_field"],
                column_renaming={"int32_field_original": "int32_field"},
            )
            .inputs([1, 2], type=TheRow)
            .output(0, type=TheRow, schema=self._first_output_schema)
            .outputs(range(1, 6), type=TheRow)
        )

    def __call__(self, row, context):
        yield OutputRow(row, table_index=context.get_table_index())


class TwoInputReducer(TypedJob):
    def prepare_operation(self, context, preparer):
        preparer.input(0, type=TheRow).input(1, type=Row1).output(0, type=TheRow)

    def __call__(self, rows):
        for row, ctx in rows.with_context():
            if ctx.get_table_index() == 0:
                assert isinstance(row, TheRow)
                yield row
            else:
                assert ctx.get_table_index() == 1
                assert isinstance(row, Row1)
                yield TheRow(
                    int32_field=row.int32_field,
                    str_field=row.str_field,
                    struct_field=Struct(str_field="foo", uint8_field=12),
                )


class SummingReducer(TypedJob):
    def prepare_operation(self, context, preparer):
        preparer.input(0, type=TheRow).output(0, type=SumRow)

    def __call__(self, rows):
        s = 0
        key = None
        for row in rows:
            key = row.str_field
            s += row.int32_field
        assert key is not None
        yield SumRow(key=key, sum=s)


class TwoInputSummingReducer(TypedJob):
    def prepare_operation(self, context, preparer):
        preparer.input(0, type=TheRow).input(1, type=Row1).output(0, type=SumRow)

    def __call__(self, rows):
        s = 0
        key = None
        for row, ctx in rows.with_context():
            if ctx.get_table_index() == 0:
                assert isinstance(row, TheRow)
            else:
                assert ctx.get_table_index() == 1
                assert isinstance(row, Row1)
            key = row.str_field
            s += row.int32_field
        assert key is not None
        yield SumRow(key=key, sum=s)


TWO_OUTPUT_MAPPER_ROWS_SECOND_TABLE = [
    {
        "int32_field": -(2 ** 31) + 1,
        "str_field": "Mapped: Привет, мир",
    },
    {
        "int32_field": -123 + 1,
        "str_field": "Mapped: spam",
    },
]


TWO_INPUT_REDUCER_OUTPUT_ROWS = ROW_DICTS + [
    {
        "int32_field": -(2 ** 31) + 1,
        "str_field": "Mapped: Привет, мир",
        "struct_field": {
            "str_field": "foo",
            "uint8_field": 12,
        },
    },
    {
        "int32_field": -123 + 1,
        "str_field": "Mapped: spam",
        "struct_field": {
            "str_field": "foo",
            "uint8_field": 12,
        },
    },
]


@pytest.mark.usefixtures("yt_env_v4")
class TestTypedApi(object):
    @authors("levysotsky")
    def test_basic_read_and_write(self):
        table = "//tmp/table"
        yt.remove(table, force=True)
        yt.create("table", table, attributes={"schema": TableSchema.from_row_type(TheRow)})
        yt.write_table(table, ROW_DICTS)
        iterator = yt.read_table_structured(table, TheRow)
        row = next(iterator)
        assert iterator.get_row_index() == 0
        assert iterator.get_table_index() == 0
        assert iterator.get_key_switch() is False
        assert row == ROWS[0]

        row = next(iterator)
        assert iterator.get_row_index() == 1
        assert iterator.get_table_index() == 0
        assert iterator.get_key_switch() is False
        assert row == ROWS[1]

        with pytest.raises(StopIteration):
            next(iterator)

        yt.write_table_structured(table, TheRow, ROWS)
        read_rows = list(yt.read_table(table))
        assert read_rows == ROW_DICTS
        yt.write_table_structured(table, TheRow, [])
        read_rows = list(yt.read_table(table))
        assert read_rows == []

    @authors("levysotsky")
    @pytest.mark.parametrize("create", [True, False])
    def test_write_schema_inference(self, create):
        table = "//tmp/table"
        yt.remove(table, force=True)

        if create:
            yt.create("table", table)
            assert TableSchema.from_yson_type(yt.get(table + "/@schema")) == TableSchema(strict=False)

        yt.write_table_structured(table, TheRow, ROWS)

        expected_schema = TableSchema() \
            .add_column("int32_field", ti.Int64) \
            .add_column("str_field", ti.Utf8) \
            .add_column("struct_field", ti.Optional[ti.Struct[
                "str_field": ti.Utf8,
                "uint8_field": ti.Optional[ti.Uint64],
            ]])

        assert TableSchema.from_row_type(TheRow) == expected_schema
        assert TableSchema.from_yson_type(yt.get(table + "/@schema")) == expected_schema

    @authors("levysotsky")
    def test_read_ranges(self):
        table = "//tmp/table"
        yt.remove(table, force=True)
        yt.create("table", table, attributes={"schema": TableSchema.from_row_type(TheRow)})
        row_count = 100
        generator = ({"int32_field": i, "str_field": str(i)} for i in range(row_count))
        yt.write_table(table, generator)
        ranges = [(1, 4), (4, 5), (10, 20), (7, 10), (90, 100)]
        ranges_dicts = [
            {
                "lower_limit": {"row_index": begin},
                "upper_limit": {"row_index": end},
            }
            for begin, end in ranges
        ]
        path_with_ranges = yt.TablePath(table, ranges=ranges_dicts)
        iterator = yt.read_table_structured(path_with_ranges, TheRow)
        for range_index, (begin, end) in enumerate(ranges):
            for row_index in range(begin, end):
                row = next(iterator)
                assert iterator.get_row_index() == row_index
                assert iterator.get_table_index() == 0
                assert iterator.get_key_switch() is False
                assert iterator.get_range_index() == range_index
                assert row == TheRow(int32_field=row_index, str_field=str(row_index))
        with pytest.raises(StopIteration):
            next(iterator)

        iterator = yt.read_table_structured(path_with_ranges, TheRow).with_context()
        for range_index, (begin, end) in enumerate(ranges):
            for row_index in range(begin, end):
                row, context = next(iterator)
                assert context.get_row_index() == row_index
                assert context.get_table_index() == 0
                assert context.get_key_switch() is False
                assert context.get_range_index() == range_index
                assert row == TheRow(int32_field=row_index, str_field=str(row_index))
        with pytest.raises(StopIteration):
            next(iterator)

    @authors("levysotsky")
    def test_map_with_context(self):
        input_tables = ["//tmp/input1", "//tmp/input2"]
        output_table = "//tmp/output"
        schema = TableSchema.from_row_type(TheRow)
        for input_table in input_tables:
            yt.remove(input_table, force=True)
            yt.create("table", input_table, attributes={"schema": schema})
            yt.write_table(input_table, ROW_DICTS)

        yt.run_map(
            MapperWithContext(),
            input_tables,
            output_table,
            spec={"max_failed_job_count": 1},
        )

        expected_rows = []
        for table_index in range(2):
            for row_index, row in enumerate(ROW_DICTS):
                row_copy = copy.deepcopy(row)
                row_copy.update({
                    "row_index": row_index,
                    "table_index": table_index,
                })
                expected_rows.append(row_copy)
        read_rows = sorted(
            yt.read_table(output_table),
            key=lambda row: (row["table_index"], row["row_index"]),
        )
        assert read_rows == expected_rows

    @authors("levysotsky")
    @pytest.mark.parametrize("aggregator", [True, False])
    def test_map_two_outputs(self, aggregator):
        input_table = "//tmp/input"
        output_table1 = "//tmp/output1"
        output_table2 = "//tmp/output2"
        yt.remove(input_table, force=True)
        schema = TableSchema.from_row_type(TheRow)
        yt.create("table", input_table, attributes={"schema": schema})
        yt.write_table(input_table, ROW_DICTS)

        if aggregator:
            mapper = AggregatorTwoOutputMapper()
        else:
            mapper = TwoOutputMapper(for_map_reduce=False)

        yt.run_map(
            mapper,
            input_table,
            [output_table1, output_table2],
            spec={"max_failed_job_count": 1},
        )
        read_rows1 = list(yt.read_table(output_table1))
        assert read_rows1 == ROW_DICTS
        read_rows2 = list(yt.read_table(output_table2))
        assert read_rows2 == TWO_OUTPUT_MAPPER_ROWS_SECOND_TABLE

    @authors("levysotsky")
    @pytest.mark.parametrize("trivial_mapper", [True, False])
    def test_basic_map_reduce(self, trivial_mapper):
        input_table = "//tmp/input"
        output_table = "//tmp/output"
        yt.remove(input_table, force=True)
        schema = TableSchema.from_row_type(TheRow)
        yt.create("table", input_table, attributes={"schema": schema})
        yt.write_table(input_table, ROW_DICTS)
        yt.run_map_reduce(
            mapper=IdentityMapper() if not trivial_mapper else None,
            reducer=IdentityReducer(),
            source_table=input_table,
            destination_table=output_table,
            reduce_by=["int32_field"],
            spec={"max_failed_job_count": 1},
        )
        read_rows = list(yt.read_table(output_table))
        assert read_rows == ROW_DICTS

    @authors("levysotsky")
    def test_map_reduce_several_intermediate_streams(self):
        input_table = "//tmp/input"
        output_table = "//tmp/output"
        yt.remove(input_table, force=True)
        schema = TableSchema.from_row_type(TheRow)
        yt.create("table", input_table, attributes={"schema": schema})
        yt.write_table(input_table, ROW_DICTS)
        yt.run_map_reduce(
            mapper=TwoOutputMapper(for_map_reduce=True),
            reducer=TwoInputReducer(),
            source_table=input_table,
            destination_table=output_table,
            reduce_by=["int32_field"],
            spec={"max_failed_job_count": 1},
        )
        read_rows = list(yt.read_table(output_table))

        def sort_key(d):
            return (d["int32_field"], "struct_field" not in d)

        assert sorted(read_rows, key=sort_key) == sorted(TWO_INPUT_REDUCER_OUTPUT_ROWS, key=sort_key)

    @authors("levysotsky")
    def test_map_reduce_trivial_mapper_several_intermediate_streams(self):
        input_table1 = "//tmp/input1"
        input_table2 = "//tmp/input2"
        output_table = "//tmp/output"

        yt.remove(input_table1, force=True)
        yt.create_table(input_table1, attributes={"schema": TableSchema.from_row_type(TheRow)})
        yt.write_table(input_table1, ROW_DICTS)

        yt.remove(input_table2, force=True)
        yt.create_table(input_table2, attributes={"schema": TableSchema.from_row_type(Row1)})
        yt.write_table(input_table2, TWO_OUTPUT_MAPPER_ROWS_SECOND_TABLE)

        yt.run_map_reduce(
            mapper=None,
            reducer=TwoInputReducer(),
            source_table=[input_table1, input_table2],
            destination_table=output_table,
            reduce_by=["int32_field"],
            spec={"max_failed_job_count": 1},
        )
        read_rows = list(yt.read_table(output_table))

        def sort_key(d):
            return (d["int32_field"], "struct_field" not in d)

        assert sorted(read_rows, key=sort_key) == sorted(TWO_INPUT_REDUCER_OUTPUT_ROWS, key=sort_key)

    def _do_test_grouping(self, input_rows, row_types, expected_output_rows, reducer):
        table_count = len(input_rows)
        input_tables = ["//tmp/input_{}".format(i) for i in range(table_count)]
        for input_table, row_type, rows in zip(input_tables, row_types, input_rows):
            yt.remove(input_table, force=True)
            yt.create_table(input_table, attributes={"schema": TableSchema.from_row_type(row_type)})
            yt.write_table(input_table, rows)
        output_table = "//tmp/output"

        def sort_key(d):
            return d["key"]

        yt.run_map_reduce(
            mapper=IdentityMapper(row_types),
            reducer=reducer,
            source_table=input_tables,
            destination_table=output_table,
            reduce_by=["str_field"],
            spec={"max_failed_job_count": 1},
        )

        assert sorted(yt.read_table(output_table), key=sort_key) == expected_output_rows

        yt.run_map_reduce(
            mapper=None,
            reducer=reducer,
            source_table=input_tables,
            destination_table=output_table,
            reduce_by=["str_field"],
            spec={"max_failed_job_count": 1},
        )

        assert sorted(yt.read_table(output_table), key=sort_key) == expected_output_rows

        for input_table in input_tables:
            yt.run_sort(input_table, sort_by="str_field")

        yt.run_reduce(
            reducer,
            source_table=input_tables,
            destination_table=output_table,
            reduce_by=["str_field"],
            spec={"max_failed_job_count": 1},
        )

        assert sorted(yt.read_table(output_table), key=sort_key) == expected_output_rows

    @authors("levysotsky")
    def test_grouping_single_input(self):
        rows = [
            [
                {"int32_field": 10, "str_field": "b"},
                {"int32_field": 10, "str_field": "b"},
                {"int32_field": 10, "str_field": "c"},
                {"int32_field": 10, "str_field": "a"},
                {"int32_field": 10, "str_field": "a"},
                {"int32_field": 10, "str_field": "a"},
            ],
        ]

        expected_rows = [
            {"key": "a", "sum": 30},
            {"key": "b", "sum": 20},
            {"key": "c", "sum": 10},
        ]

        self._do_test_grouping(rows, [TheRow], expected_rows, SummingReducer())

    @authors("levysotsky")
    def test_grouping_two_inputs(self):
        rows = [
            [
                {"int32_field": 10, "str_field": "b"},
                {"int32_field": 10, "str_field": "b"},
                {"int32_field": 10, "str_field": "c"},
                {"int32_field": 10, "str_field": "a"},
                {"int32_field": 10, "str_field": "a"},
                {"int32_field": 10, "str_field": "a"},
            ],
            [
                {"int32_field": 100, "str_field": "c"},
                {"int32_field": 100, "str_field": "c"},
                {"int32_field": 100, "str_field": "a"},
                {"int32_field": 100, "str_field": "a"},
            ],
        ]

        expected_rows = [
            {"key": "a", "sum": 230},
            {"key": "b", "sum": 20},
            {"key": "c", "sum": 210},
        ]

        self._do_test_grouping(rows, [TheRow, Row1], expected_rows, TwoInputSummingReducer())

    @authors("levysotsky")
    def test_grouping_empty_input(self):
        rows = [
            {"int32_field": 100, "str_field": "a"},
            {"int32_field": 100, "str_field": "a"},
            {"int32_field": 100, "str_field": "z"},
            {"int32_field": 100, "str_field": "z"},
        ]

        input_table = "//tmp/input"
        schema = TableSchema.from_row_type(TheRow).build_schema_sorted_by(["str_field"])
        yt.create_table(input_table, attributes={"schema": schema})
        yt.write_table(input_table, rows)
        output_table = "//tmp/output"

        # The range is crafted intentionally to result in empty set of rows.
        range_ = {
            "lower_limit": {"key": ["p"]},
            "upper_limit": {"key": ["q"]},
        }
        yt.run_reduce(
            EmptyInputReducer(),
            source_table=yt.TablePath(input_table, ranges=[range_]),
            destination_table=output_table,
            spec={"max_failed_job_count": 1},
            reduce_by=["str_field"],
        )

        expected_rows = [
            ROW_DICTS[0],
        ]
        assert list(yt.read_table(output_table)) == expected_rows

    @authors("levysotsky")
    @pytest.mark.parametrize("other_columns_as_bytes", [True, False])
    def test_other_columns(self, other_columns_as_bytes):
        dict_row = {
            "x": 10,
            "y": {"a": "foo", "b": b"bar"},
            "z": -10,
        }
        table = "//tmp/table"
        schema = (
            TableSchema()
            .add_column("x", ti.Int64)
            .add_column("y", ti.Struct["a": ti.Utf8, "b": ti.String])
            .add_column("z", ti.Int64)
        )
        yt.create("table", table, attributes={"schema": schema})
        yt.write_table(table, [dict_row])

        other_columns_dict = {
            "y": {"a": "foo", "b": b"bar"},
        }
        if other_columns_as_bytes:
            other_columns = OtherColumns(yson.dumps(other_columns_dict))
        else:
            other_columns = OtherColumns(other_columns_dict)
        assert len(other_columns) == 1
        assert other_columns["y"]["a"] == "foo"
        if other_columns_as_bytes:
            assert other_columns["y"]["b"] == "bar"
        else:
            assert other_columns["y"]["b"] == b"bar"
        other_columns["z"] = -10
        assert len(other_columns) == 2
        assert other_columns["z"] == -10

        other_columns_dict["z"] = -10
        if other_columns_as_bytes:
            other_columns = OtherColumns(yson.dumps(other_columns_dict))

        @yt_dataclass
        class TheRow:
            x: int
            other: OtherColumns

        structured_row = TheRow(
            x=10,
            other=other_columns,
        )

        read_structured_rows = list(yt.read_table_structured(table, TheRow))
        assert len(read_structured_rows) == 1
        read_structured_row = read_structured_rows[0]
        assert read_structured_row.x == 10
        # All strings are automatically decoded from UTF-8 in yson parser.
        dict_row_with_str = copy.deepcopy(dict_row)
        dict_row_with_str["y"]["b"] = dict_row_with_str["y"]["b"].decode()
        assert read_structured_row.other["y"] == dict_row_with_str["y"]
        assert read_structured_row.other["z"] == dict_row_with_str["z"]

        yt.write_table_structured(table, TheRow, [structured_row])
        read_dict_rows = list(yt.read_table(table))
        assert read_dict_rows == [dict_row_with_str]

    @authors("levysotsky")
    def test_schema_matching_read(self):
        @yt_dataclass
        class Struct:
            a1: typing.Optional[Int64]
            b1: Uint64
            c1: bytes
            d1: typing.Optional[str]
            e1: str

        @yt_dataclass
        class Row1:
            a: typing.Optional[Int64]
            b: typing.Optional[Struct]

        schema = (
            TableSchema()
            .add_column("a", ti.Int64)
            .add_column(
                "b",
                ti.Struct[
                    "c1": ti.String,
                    "b1": ti.Uint64,
                    "a1": ti.Int64,
                    "e1": ti.String,
                ],
            )
            .add_column("c", ti.Uint64)
        )

        table = "//tmp/table"
        yt.create("table", table, attributes={"schema": schema})
        row = {
            "a": 100,
            "b": {
                "a1": -100,
                "b1": yson.YsonUint64(255),
                "c1": b"\xFF",
                "e1": "Приветосы".encode(),
            },
            "c": yson.YsonUint64(1000),
        }
        yt.write_table(table, [row])
        read_rows = list(yt.read_table_structured(table, Row1))
        assert read_rows == [
            Row1(
                a=100,
                b=Struct(
                    a1=-100,
                    b1=255,
                    c1=b"\xFF",
                    d1=None,
                    e1="Приветосы",
                ),
            ),
        ]

        @yt_dataclass
        class Row2:
            a: Int64
            b: Int64

        schema = TableSchema().add_column("b", ti.Int64)
        yt.remove(table, force=True)
        yt.create_table(table, attributes={"schema": schema})
        with pytest.raises(yt.YtError, match=r'struct schema is missing non-nullable field ".*\.Row2\.a"'):
            yt.read_table_structured(table, Row2)

        schema = TableSchema().add_column("b", ti.Int64).add_column("a", ti.Optional[ti.Int64])
        yt.remove(table, force=True)
        yt.create_table(table, attributes={"schema": schema})
        with pytest.raises(yt.YtError, match=r'field ".*\.Row2\.a" is non-nullable in yt_dataclass and optional'):
            yt.read_table_structured(table, Row2)

    @authors("levysotsky")
    def test_schema_matching_write(self):
        @yt_dataclass
        class Struct:
            a1: typing.Optional[Int64]
            b1: Uint64
            c1: bytes
            d1: str
            e1: str

        @yt_dataclass
        class Row2:
            a: Int64
            b: Struct
            c: int

        schema = (
            TableSchema()
            .add_column("a", ti.Optional[ti.Int64])
            .add_column(
                "b",
                ti.Struct[
                    "d1": ti.Optional[ti.Utf8],
                    "c1": ti.String,
                    "b1": ti.Uint64,
                    "a1": ti.Optional[ti.Int64],
                    "e1": ti.String,
                ],
            )
            .add_column("c", ti.Optional[ti.Int64])
        )

        table = "//tmp/table"
        yt.create("table", table, attributes={"schema": schema})
        row = Row2(
            a=100,
            b=Struct(
                a1=-100,
                b1=255,
                c1=b"\xFF",
                d1="Енот",
                e1="Луна",
            ),
            c=1000,
        )
        yt.write_table_structured(table, Row2, [row])
        read_rows = list(yt.read_table(table))
        assert read_rows == [
            {
                "a": 100,
                "b": {
                    "a1": -100,
                    "b1": yson.YsonUint64(255),
                    "c1": b"\xFF",
                    "d1": "Енот",
                    "e1": "Луна",
                },
                "c": 1000,
            },
        ]

        schema_missing_b = (
            TableSchema()
            .add_column("a", ti.Optional[ti.Int64])
            .add_column("c", ti.Optional[ti.Int64])
        )

        yt.remove(table)
        yt.create("table", table, attributes={"schema": schema_missing_b})
        with pytest.raises(yt.YtError, match=r'struct schema is missing field ".*\.Row2\.b"'):
            yt.write_table_structured(table, Row2, [row])

        schema_with_required_extra = copy.deepcopy(schema)
        schema_with_required_extra.add_column("extra", ti.Int64)

        yt.remove(table)
        yt.create("table", table, attributes={"schema": schema_with_required_extra})
        with pytest.raises(yt.YtError, match=r'yt_dataclass is missing non-nullable field ".*\.Row2\.extra"'):
            yt.write_table_structured(table, Row2, [row])

        @yt_dataclass
        class Row3:
            a: typing.Optional[Int64]

        schema_with_required_a = TableSchema().add_column("a", ti.Int64)
        yt.remove(table)
        yt.create("table", table, attributes={"schema": schema_with_required_a})
        with pytest.raises(yt.YtError, match=r'field ".*\.Row3\.a" is optional in yt_dataclass and required'):
            yt.write_table_structured(table, Row3, [Row3(a=1)])

    @authors("levysotsky")
    def test_schema_matching_primitive(self):
        def run_read_or_write(py_type, ti_type, value, for_reading):
            @yt_dataclass
            class Row1:
                field: py_type

            schema = TableSchema().add_column("field", ti_type)
            path = "//tmp/table"
            yt.remove(path, force=True)
            yt.create_table(path, attributes={"schema": schema})
            if for_reading:
                yt.write_table(path, [{"field": value}])
                rows = list(yt.read_table_structured(path, Row1))
                assert len(rows) == 1
                return rows[0].field
            else:
                yt.write_table_structured(path, Row1, [Row1(field=value)])
                rows = list(yt.read_table(path))
                assert len(rows) == 1
                return rows[0]["field"]

        for py_type, ti_type in zip([Int8, Int16, Int32, Int64], [ti.Int8, ti.Int16, ti.Int32, ti.Int64]):
            assert -10 == run_read_or_write(py_type, ti_type, -10, for_reading=True)
            assert -10 == run_read_or_write(py_type, ti_type, -10, for_reading=False)

        for py_type, ti_type in zip([Uint8, Uint16, Uint32, Uint64], [ti.Uint8, ti.Uint16, ti.Uint32, ti.Uint64]):
            assert 10 == run_read_or_write(py_type, ti_type, 10, for_reading=True)
            assert 10 == run_read_or_write(py_type, ti_type, 10, for_reading=False)

        with pytest.raises(yt.YtError, match="signedness"):
            run_read_or_write(Int64, ti.Uint64, 10, for_reading=True)
        with pytest.raises(yt.YtError, match="signedness"):
            run_read_or_write(Uint64, ti.Int64, 10, for_reading=True)
        with pytest.raises(yt.YtError, match="signedness"):
            run_read_or_write(Int64, ti.Uint64, 10, for_reading=False)
        with pytest.raises(yt.YtError, match="signedness"):
            run_read_or_write(Uint64, ti.Int64, 10, for_reading=False)

        assert -10 == run_read_or_write(Int64, ti.Int32, -10, for_reading=True)
        with pytest.raises(yt.YtError, match="larger than destination"):
            assert -10 == run_read_or_write(Int64, ti.Int32, -10, for_reading=False)

        assert -10 == run_read_or_write(Int32, ti.Int16, -10, for_reading=True)
        with pytest.raises(yt.YtError, match="larger than destination"):
            assert -10 == run_read_or_write(Int32, ti.Int16, -10, for_reading=False)

        assert 10 == run_read_or_write(Uint64, ti.Uint32, 10, for_reading=True)
        with pytest.raises(yt.YtError, match="larger than destination"):
            assert 10 == run_read_or_write(Uint64, ti.Uint32, 10, for_reading=False)

        assert 10 == run_read_or_write(Uint32, ti.Uint16, 10, for_reading=True)
        with pytest.raises(yt.YtError, match="larger than destination"):
            assert 10 == run_read_or_write(Uint32, ti.Uint16, 10, for_reading=False)

        string = "Привет"
        string_utf8 = string.encode("utf-8")

        assert string == run_read_or_write(str, ti.Utf8, string, for_reading=True)
        assert string == run_read_or_write(str, ti.Utf8, string, for_reading=False)

        assert string == run_read_or_write(str, ti.String, string_utf8, for_reading=True)
        assert string == run_read_or_write(str, ti.String, string, for_reading=False)

        assert string_utf8 == run_read_or_write(bytes, ti.String, string_utf8, for_reading=True)
        assert string == run_read_or_write(bytes, ti.String, string_utf8, for_reading=False)

        assert string_utf8 == run_read_or_write(bytes, ti.Utf8, string_utf8, for_reading=True)
        assert string == run_read_or_write(bytes, ti.Utf8, string_utf8, for_reading=False)

    @authors("levysotsky")
    def test_prepare_operation(self):
        first_input_schema = (
            TableSchema()
            .add_column("int32_field_original", ti.Int32)
            .add_column("str_field", ti.Utf8)
            .add_column("struct_field", ti.Struct["str_field" : ti.Utf8, "uint8_field" : ti.Optional[ti.Uint64]])
            .add_column("extra_field", ti.Int64)
        )
        other_input_schema = TableSchema.from_row_type(TheRow)

        first_output_schema = TableSchema.from_row_type(TheRow)
        first_output_schema.add_column("extra_field", ti.Optional[ti.Int64])
        other_output_schema = TableSchema.from_row_type(TheRow)

        input_paths = []
        for i in range(3):
            table = "//tmp/in_{}".format(i)
            input_paths.append(table)
            if i == 0:
                schema = first_input_schema
            else:
                schema = other_input_schema
            yt.create("table", table, attributes={"schema": schema})
            row = {
                "str_field": "Хы",
                "struct_field": {
                    "str_field": "Зы",
                },
            }
            if i == 0:
                row["int32_field_original"] = 42
                row["extra_field"] = -42
            else:
                row["int32_field"] = 42
            yt.write_table(table, [row])

        output_paths = ["//tmp/out_{}".format(i) for i in range(6)]

        mapper = MapperWithBigPrepareOperation(first_output_schema)
        yt.run_map(mapper, input_paths, output_paths)

        actual_output_schemas = [TableSchema.from_yson_type(yt.get(output_paths[i] + "/@schema")) for i in range(6)]
        assert actual_output_schemas[0] == first_output_schema
        for i in range(1, 6):
            assert actual_output_schemas[i] == other_output_schema

        for i in range(3):
            rows = list(yt.read_table(output_paths[i]))
            expected_row = {
                "int32_field": 42,
                "str_field": "Хы",
                "struct_field": {
                    "str_field": "Зы",
                    "uint8_field": None,
                },
            }
            if i == 0:
                expected_row["extra_field"] = None
            assert rows == [expected_row]
        for i in range(3, 6):
            assert list(yt.read_table(output_paths[i])) == []

    @authors("levysotsky")
    def test_prepare_operation_errors(self):
        input_tables, output_tables = [], []
        for i in range(3):
            input_table = "//tmp/in_{}".format(i)
            input_tables.append(input_table)
            output_table = "//tmp/out_{}".format(i)
            output_tables.append(output_table)
            yt.remove(input_table, force=True)
            schema = TableSchema.from_row_type(TheRow)
            yt.create("table", input_table, attributes={"schema": schema})
            yt.write_table(input_table, ROW_DICTS)

        class TypedJobNotToBeCalled(TypedJob):
            pass

        class MapperWithMissingInput(TypedJobNotToBeCalled):
            def prepare_operation(self, context, preparer):
                preparer.inputs([0, 2], type=TheRow).output(0, type=TheRow)

        with pytest.raises(ValueError, match="Missing type for input table no. 1 \\(//tmp/in_1\\)"):
            yt.run_map(MapperWithMissingInput(), input_tables, output_tables)

        class MapperWithMissingOutput(TypedJobNotToBeCalled):
            def prepare_operation(self, context, preparer):
                preparer.inputs(range(3), type=TheRow).outputs([0, 1], type=TheRow)

        with pytest.raises(ValueError, match="Missing type for output table no. 2 \\(//tmp/out_2\\)"):
            yt.run_map(MapperWithMissingOutput(), input_tables, output_tables)

        class MapperWithOutOfRange(TypedJobNotToBeCalled):
            def prepare_operation(self, context, preparer):
                preparer.inputs(range(100), type=TheRow).output(range(100), type=TheRow)

        with pytest.raises(ValueError, match="Input type index 3 out of range \\[0, 3\\)"):
            yt.run_map(MapperWithOutOfRange(), input_tables, output_tables)

        class MapperWithTypes(TypedJobNotToBeCalled):
            def __init__(self, input_type, output_type):
                self._input_type = input_type
                self._output_type = output_type
            def prepare_operation(self, context, preparer):
                preparer.inputs(range(3), type=self._input_type).outputs(range(3), type=self._output_type)

        with pytest.raises(TypeError, match="Input type must be a class marked with"):
            yt.run_map(MapperWithTypes(int, TheRow), input_tables, output_tables)
        with pytest.raises(TypeError, match="Output type must be a class marked with"):
            yt.run_map(MapperWithTypes(TheRow, int), input_tables, output_tables)

        @yt_dataclass
        class Row2:
            list_field: typing.List[typing.List[int]]

        with pytest.raises(yt.YtError, match=r'struct schema is missing non-nullable field ".*\.Row2\.list_field"'):
            yt.run_map(MapperWithTypes(Row2, TheRow), input_tables, output_tables)

        with pytest.raises(yt.YtError, match=r'incompatible with "input_query"'):
            yt.run_map(MapperWithTypes(Row2, TheRow), input_tables, output_tables, spec={"input_query": "*"})

    @authors("ignat")
    def test_yson_bytes(self):
        @yt_dataclass
        class RowWithYson:
            pure_yson: typing.Optional[YsonBytes]
            list_of_ysons: typing.List[typing.Optional[YsonBytes]]

        schema = TableSchema.from_row_type(RowWithYson)

        table = "//tmp/table"
        yt.create("table", table, attributes={"schema": schema})
        yt.write_table_structured(
            table,
            RowWithYson,
            [
                RowWithYson(pure_yson=b"1",
                            list_of_ysons=[
                                yson.dumps({"my_data": 10}),
                                yson.dumps(["item1", yson.loads(b"<attr=10>item2")]),
                            ]),
            ])

        typed_rows = list(yt.read_table_structured(table, RowWithYson))
        assert len(typed_rows) == 1
        row = typed_rows[0]
        assert yson.loads(row.pure_yson) == 1
        assert len(row.list_of_ysons) == 2
        assert yson.loads(row.list_of_ysons[0]) == {"my_data": 10}
        assert yson.loads(row.list_of_ysons[1]) == ["item1", yson.loads(b"<attr=10>item2")]

        rows = list(yt.read_table(table))
        assert len(rows) == 1
        row = rows[0]
        assert row["pure_yson"] == 1
        assert len(row["list_of_ysons"]) == 2
        assert row["list_of_ysons"][0] == {"my_data": 10}
        assert row["list_of_ysons"][1] == ["item1", yson.loads(b"<attr=10>item2")]

    @authors("ignat")
    def test_dict(self):
        @yt_dataclass
        class RowWithDicts:
            dict_str_to_int: typing.Optional[typing.Dict[str, int]]
            dict_int_to_bytes: typing.Optional[typing.Dict[int, bytes]]

        schema = TableSchema.from_row_type(RowWithDicts)

        table = "//tmp/table"
        yt.create("table", table, attributes={"schema": schema})
        yt.write_table_structured(
            table,
            RowWithDicts,
            [
                RowWithDicts(
                    dict_str_to_int={"a": 10, "b": 20},
                    dict_int_to_bytes={1: b"\x01", 2: b"\x02"},
                )
            ])

        typed_rows = list(yt.read_table_structured(table, RowWithDicts))
        assert len(typed_rows) == 1
        row = typed_rows[0]
        assert row.dict_str_to_int == {"a": 10, "b": 20}
        assert row.dict_int_to_bytes == {1: b"\x01", 2: b"\x02"}
