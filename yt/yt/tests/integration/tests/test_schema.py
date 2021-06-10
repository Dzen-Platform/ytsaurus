# -*- coding: utf8 -*-

from yt_env_setup import YTEnvSetup

from yt_commands import (  # noqa
    authors, print_debug, wait, wait_assert, wait_breakpoint, release_breakpoint, with_breakpoint,
    events_on_fs, reset_events_on_fs,
    create, ls, get, set, copy, move, remove, link, exists,
    create_account, create_network_project, create_tmpdir, create_user, create_group,
    create_pool, create_pool_tree, remove_pool_tree,
    create_data_center, create_rack, create_table,
    make_ace, check_permission, add_member,
    make_batch_request, execute_batch, get_batch_error,
    start_transaction, abort_transaction, commit_transaction, lock,
    insert_rows, select_rows, lookup_rows, delete_rows, trim_rows, alter_table,
    read_file, write_file, read_table, write_table, write_local_file,
    map, reduce, map_reduce, join_reduce, merge, vanilla, sort, erase, remote_copy,
    run_test_vanilla, run_sleeping_vanilla,
    abort_job, list_jobs, get_job, abandon_job, interrupt_job,
    get_job_fail_context, get_job_input, get_job_stderr, get_job_spec,
    dump_job_context, poll_job_shell,
    abort_op, complete_op, suspend_op, resume_op,
    get_operation, list_operations, clean_operations,
    get_operation_cypress_path, scheduler_orchid_pool_path,
    scheduler_orchid_default_pool_tree_path, scheduler_orchid_operation_path,
    scheduler_orchid_default_pool_tree_config_path, scheduler_orchid_path,
    scheduler_orchid_node_path, scheduler_orchid_pool_tree_config_path, scheduler_orchid_pool_tree_path,
    mount_table, wait_for_tablet_state,
    sync_create_cells, sync_mount_table, sync_unmount_table,
    sync_freeze_table, sync_unfreeze_table, sync_reshard_table,
    sync_flush_table, sync_compact_table,
    get_first_chunk_id, get_singular_chunk_id, get_chunk_replication_factor, multicell_sleep,
    update_nodes_dynamic_config, update_controller_agent_config,
    update_op_parameters, enable_op_detailed_logs,
    set_node_banned, set_banned_flag, set_account_disk_space_limit,
    check_all_stderrs,
    create_test_tables, create_dynamic_table, PrepareTables,
    get_statistics,
    make_random_string, raises_yt_error,
    build_snapshot,
    get_driver, Driver, execute_command)

from decimal_helpers import decode_decimal, encode_decimal, YtNaN, MAX_DECIMAL_PRECISION

from yt_type_helpers import (
    make_schema, normalize_schema, make_sorted_column, make_column,
    optional_type, list_type, dict_type, struct_type, tuple_type, variant_tuple_type, variant_struct_type,
    decimal_type, tagged_type)

import yt_error_codes

from yt.common import YtError
import yt.yson as yson

import pytest

import collections
import decimal
import json
import random


# Run our tests on all decimal precisions might be expensive so we create
# representative sample of possible precisions.
INTERESTING_DECIMAL_PRECISION_LIST = [
    1, 5, 9,  # 4 bytes
    10, 15, 18,  # 8 bytes
    19, 25, MAX_DECIMAL_PRECISION,  # 16 bytes
]

POSITIONAL_YSON = yson.loads("<complex_type_mode=positional>yson")

##################################################################


def stable_json(obj):
    return json.dumps(obj, sort_keys=True)


def tx_write_table(*args, **kwargs):
    """
    Write rows to table transactionally.

    If write_table fails with some error it is not guaranteed that table is not locked.
    Locks can linger for some time and prevent from working with this table.

    This function avoids such lingering locks by explicitly creating external transaction
    and aborting it explicitly in case of error.
    """
    parent_tx = kwargs.pop("tx", "0-0-0-0")
    timeout = kwargs.pop("timeout", 60000)

    try:
        tx = start_transaction(timeout=timeout, tx=parent_tx)
    except Exception as e:
        raise AssertionError("Cannot start transaction: {}".format(e))

    try:
        write_table(*args, tx=tx, **kwargs)
    except YtError:
        try:
            abort_transaction(tx)
        except Exception as e:
            raise AssertionError("Cannot abort wrapper transaction: {}".format(e))
        raise

    commit_transaction(tx)


class TypeTester(object):
    class DynamicHelper(object):
        def make_schema(self, type_v3):
            return make_schema(
                [
                    make_sorted_column("key", "int64"),
                    make_column("column", type_v3),
                ],
                unique_keys=True,
            )

        def write(self, path, value):
            insert_rows(path, [{"key": 0, "column": value}])

    class StaticHelper(object):
        def make_schema(self, type_v3):
            return make_schema([make_column("column", type_v3)])

        def write(self, path, value):
            tx_write_table(path, [{"column": value}], input_format=POSITIONAL_YSON)

    def __init__(self, type_list, dynamic=False):
        self.types = {}
        if dynamic:
            self._helper = self.DynamicHelper()
        else:
            self._helper = self.StaticHelper()

        for i, t in enumerate(type_list):
            path = "//tmp/table{}".format(i)
            self.types[stable_json(t)] = path
            create(
                "table",
                path,
                force=True,
                attributes={
                    "schema": self._helper.make_schema(t),
                    "dynamic": dynamic,
                },
            )

        if dynamic:
            for p in self.types.values():
                mount_table(p)
            for p in self.types.values():
                wait_for_tablet_state(path, "mounted")

    def check_good_value(self, logical_type, value):
        path = self.types.get(stable_json(logical_type), None)
        if path is None:
            raise ValueError("type is unknown")
        self._helper.write(path, value)

    def check_bad_value(self, logical_type, value):
        with raises_yt_error(yt_error_codes.SchemaViolation):
            self.check_good_value(logical_type, value)

    def check_conversion_error(self, logical_type, value):
        with raises_yt_error("Unable to convert"):
            self.check_good_value(logical_type, value)


class SingleColumnTable(object):
    def __init__(self, column_type, optimize_for, path="//tmp/table"):
        self.path = path
        create(
            "table",
            self.path,
            force=True,
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema([make_column("column", column_type)], strict=True, unique_keys=False),
            },
        )

    def check_good_value(self, value):
        tx_write_table(self.path, [{"column": value}], input_format=POSITIONAL_YSON)

    def check_bad_value(self, value):
        with raises_yt_error(yt_error_codes.SchemaViolation):
            self.check_good_value(value)


TypeV1 = collections.namedtuple("TypeV1", ["type", "required"])


def type_v3_to_type_v1(type_v3):
    table = "//tmp/type_v3_to_type_v1_helper"
    create(
        "table",
        table,
        force=True,
        attributes={
            "schema": make_schema(
                [make_column("column", type_v3)],
                strict=True,
                unique_keys=False,
            )
        },
    )
    column_schema = get(table + "/@schema/0")
    remove(table)
    return TypeV1(column_schema["type"], column_schema["required"])


@authors("ermolovd")
@pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
class TestComplexTypes(YTEnvSetup):
    @authors("ermolovd")
    def test_complex_optional(self, optimize_for):
        type_v3 = optional_type(optional_type("int8"))
        assert type_v3_to_type_v1(type_v3) == TypeV1("any", False)

        test_table = SingleColumnTable(type_v3, optimize_for)
        test_table.check_good_value(None)
        test_table.check_good_value([None])
        test_table.check_good_value([-42])

        test_table.check_bad_value([])
        test_table.check_bad_value([257])

    @authors("ermolovd")
    def test_struct(self, optimize_for):
        type_v3 = struct_type(
            [
                ("a", "utf8"),
                ("b", optional_type("int64")),
            ]
        )
        assert type_v3_to_type_v1(type_v3) == TypeV1("any", True)

        test_table = SingleColumnTable(type_v3, optimize_for)
        test_table.check_good_value(["one", 1])
        test_table.check_good_value(["two", None])
        test_table.check_good_value(["three"])

        test_table.check_bad_value([])
        test_table.check_bad_value(None)
        test_table.check_bad_value(["one", 2, 3])
        test_table.check_bad_value(["bar", "baz"])

    @authors("ermolovd")
    def test_malformed_struct(self, optimize_for):
        with raises_yt_error("Name of struct field #0 is empty"):
            SingleColumnTable(
                struct_type(
                    [
                        ("", "int64"),
                    ]
                ),
                optimize_for,
            )

    @authors("ermolovd")
    def test_list(self, optimize_for):
        type_v3 = list_type(optional_type("string"))

        assert type_v3_to_type_v1(type_v3) == TypeV1("any", True)

        test_table = SingleColumnTable(type_v3, optimize_for)
        test_table.check_good_value([])
        test_table.check_good_value(["one"])
        test_table.check_good_value(["one", "two"])
        test_table.check_good_value(["one", "two", None])

        test_table.check_bad_value(None)
        test_table.check_bad_value({})
        test_table.check_bad_value([1, None])

    @authors("ermolovd")
    def test_tuple(self, optimize_for):
        type_v3 = tuple_type(["utf8", optional_type("int64")])
        assert type_v3_to_type_v1(type_v3) == TypeV1("any", True)

        test_table = SingleColumnTable(type_v3, optimize_for)
        test_table.check_good_value(["one", 1])
        test_table.check_good_value(["two", None])

        test_table.check_bad_value(["three"])
        test_table.check_bad_value([])
        test_table.check_bad_value(None)
        test_table.check_bad_value(["one", 2, 3])
        test_table.check_bad_value(["bar", "baz"])

    @pytest.mark.parametrize(
        "logical_type",
        [
            variant_tuple_type(["utf8", optional_type("int64")]),
            variant_struct_type([("a", "utf8"), ("b", optional_type("int64"))]),
        ],
    )
    @authors("ermolovd")
    def test_variant(self, logical_type, optimize_for):
        assert type_v3_to_type_v1(logical_type) == TypeV1("any", True)

        test_table = SingleColumnTable(logical_type, optimize_for)
        test_table.check_good_value([0, "foo"])
        test_table.check_good_value([1, None])
        test_table.check_good_value([1, 42])

        test_table.check_bad_value(None)
        test_table.check_bad_value([])
        test_table.check_bad_value(["three"])
        test_table.check_bad_value([0, "one", 2])
        test_table.check_bad_value([1, 3.14])
        test_table.check_bad_value([2, None])

    @pytest.mark.parametrize("null_type", ["null", "void"])
    @authors("ermolovd")
    def test_null_type(self, optimize_for, null_type):
        def check_schema():
            column_schema = get("//tmp/table/@schema/0")
            assert not column_schema["required"]
            assert column_schema["type"] == null_type
            assert column_schema["type_v3"] == null_type

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {
                            "name": "column",
                            "type_v3": null_type,
                        }
                    ]
                ),
            },
        )
        check_schema()

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "column",
                            "type": null_type,
                        }
                    ]
                )
            },
        )
        check_schema()

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {
                            "name": "column",
                            "type": null_type,
                            "required": False,
                        }
                    ]
                ),
            },
        )
        check_schema()

        # no exception
        tx_write_table("//tmp/table", [{}, {"column": None}])
        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/table", [{"column": 0}])

        with raises_yt_error("Null type cannot be required"):
            create(
                "table",
                "//tmp/table",
                force=True,
                attributes={
                    "schema": make_schema(
                        [
                            {
                                "name": "column",
                                "type": null_type,
                                "required": True,
                            }
                        ]
                    )
                },
            )

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "column",
                            "type_v3": list_type(null_type),
                        }
                    ]
                )
            },
        )
        tx_write_table("//tmp/table", [{"column": []}, {"column": [None]}])
        tx_write_table("//tmp/table", [{"column": []}, {"column": [None, None]}])
        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/table", [{"column": [0]}])

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={"schema": make_schema([{"name": "column", "type_v3": optional_type(null_type)}])},
        )
        tx_write_table("//tmp/table", [{"column": None}, {"column": [None]}])

        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/table", [{"column": []}])

        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/table", [{"column": []}])

    @authors("ermolovd")
    def test_dict(self, optimize_for):
        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "column",
                            "type_v3": dict_type(optional_type("string"), "int64"),
                        }
                    ]
                ),
                "optimize_for": optimize_for,
            },
        )
        assert get("//tmp/table/@schema/0/type") == "any"
        assert get("//tmp/table/@schema/0/required")

        tx_write_table(
            "//tmp/table",
            [
                {"column": []},
                {"column": [["one", 1]]},
                {"column": [["one", 1], ["two", 2]]},
                {"column": [[None, 1], [None, 2]]},
            ],
        )

        def check_bad(value):
            with raises_yt_error(yt_error_codes.SchemaViolation):
                tx_write_table(
                    "//tmp/table",
                    [
                        {"column": value},
                    ],
                )

        check_bad(None)
        check_bad({})
        check_bad(["one", 1])
        check_bad([["one"]])
        check_bad([["one", 1, 1]])
        check_bad([["one", None]])

    @authors("ermolovd")
    def test_tagged(self, optimize_for):
        logical_type1 = struct_type(
            [
                ("a", tagged_type("yt.cluster_name", "utf8")),
                ("b", optional_type("int64")),
            ]
        )
        assert type_v3_to_type_v1(logical_type1) == TypeV1("any", True)

        table1 = SingleColumnTable(logical_type1, optimize_for)

        table1.check_good_value(["hume", 1])
        table1.check_good_value(["freud", None])
        table1.check_good_value(["hahn"])

        table1.check_bad_value([])
        table1.check_bad_value(None)
        table1.check_bad_value(["sakura", 2, 3])
        table1.check_bad_value(["betula", "redwood"])

        logical_type2 = tagged_type("even", optional_type("int64"))
        assert type_v3_to_type_v1(logical_type2) == TypeV1("int64", False)

        table2 = SingleColumnTable(logical_type2, optimize_for)

        table2.check_good_value(0)
        table2.check_good_value(2)
        table2.check_good_value(None)

        table2.check_bad_value("1")
        table2.check_bad_value(3.0)

    @authors("ermolovd")
    def test_decimal(self, optimize_for):
        assert type_v3_to_type_v1(decimal_type(3, 2)) == TypeV1("string", True)

        table = SingleColumnTable(
            decimal_type(3, 2),
            optimize_for)
        table.check_good_value(encode_decimal("3.12", 3, 2))
        table.check_good_value(encode_decimal("-2.7", 3, 2))
        table.check_good_value(encode_decimal("Nan", 3, 2))
        table.check_good_value(encode_decimal("Inf", 3, 2))
        table.check_good_value(encode_decimal("-Inf", 3, 2))

        table.check_bad_value(encode_decimal("43.12", 3, 2))
        table.check_bad_value(3.14)

        table = SingleColumnTable(
            optional_type(decimal_type(3, 2)),
            optimize_for)
        table.check_good_value(encode_decimal("3.12", 3, 2))
        table.check_good_value(encode_decimal("-2.7", 3, 2))
        table.check_bad_value(encode_decimal("43.12", 3, 2))
        table.check_bad_value(3.14)
        table.check_good_value(None)

        table = SingleColumnTable(
            list_type(decimal_type(3, 2)),
            optimize_for)
        table.check_good_value([encode_decimal("3.12", 3, 2)])
        table.check_bad_value([encode_decimal("43.12", 3, 2)])
        table.check_bad_value([3.12])

    @authors("ermolovd")
    def test_uuid(self, optimize_for):
        uuid = "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xAA\xBB\xCC\xDD\xEE\xFF"
        table = SingleColumnTable("uuid", optimize_for)
        table.check_good_value(uuid)

        table.check_bad_value("")
        table.check_bad_value(uuid[:-1])
        table.check_bad_value(uuid + "a")

        table = SingleColumnTable(list_type("uuid"), optimize_for)
        table.check_good_value([uuid])
        table.check_bad_value([""])
        table.check_bad_value([uuid[:-1]])
        table.check_bad_value([uuid + "a"])


@authors("ermolovd")
class TestComplexTypesMisc(YTEnvSetup):
    NUM_SCHEDULERS = 1

    @authors("ermolovd")
    @pytest.mark.parametrize("precision", list(range(3, 36)))
    def test_decimal_various_precision(self, precision):
        table1 = SingleColumnTable(decimal_type(precision, 2), "lookup")
        table1.check_good_value(encode_decimal(decimal.Decimal("3.12"), precision, 2))
        table1.check_good_value(encode_decimal(decimal.Decimal("inf"), precision, 2))
        table1.check_good_value(encode_decimal(decimal.Decimal("nan"), precision, 2))
        table1.check_good_value(encode_decimal(decimal.Decimal("-nan"), precision, 2))
        table1.check_bad_value("")
        table1.check_bad_value("foo")
        table1.check_bad_value(None)

    @authors("ermolovd")
    @pytest.mark.parametrize("type_v3", [
        decimal_type(MAX_DECIMAL_PRECISION, 2),
        optional_type(decimal_type(MAX_DECIMAL_PRECISION, 2)),
        tagged_type("foo", decimal_type(MAX_DECIMAL_PRECISION, 2)),
        optional_type(tagged_type("foo", decimal_type(MAX_DECIMAL_PRECISION, 2))),
        tagged_type("bar", optional_type(decimal_type(MAX_DECIMAL_PRECISION, 2))),
        tagged_type("bar", optional_type(tagged_type("foo", decimal_type(MAX_DECIMAL_PRECISION, 2)))),
    ])
    def test_decimal_optional_tagged_combinations(self, type_v3):
        table1 = SingleColumnTable(type_v3, "lookup")
        table1.check_good_value(encode_decimal(decimal.Decimal("3.12"), MAX_DECIMAL_PRECISION, 2))
        table1.check_good_value(encode_decimal(decimal.Decimal("1" * 33), MAX_DECIMAL_PRECISION, 2))
        table1.check_bad_value(encode_decimal(decimal.Decimal("1" * 34), MAX_DECIMAL_PRECISION, 2))

        table1.check_bad_value(5)
        table1.check_bad_value("foo")
        table1.check_bad_value(5.5)

    @authors("ermolovd")
    def test_decimal_sort(self):
        create_table(
            "//tmp/table",
            schema=make_schema([{"name": "key", "type_v3": decimal_type(3, 2)}])
        )

        data = [
            decimal.Decimal("Infinity"),
            decimal.Decimal("-Infinity"),
            decimal.Decimal("Nan"),
            decimal.Decimal("2.71"),
            decimal.Decimal("3.14"),
            decimal.Decimal("-6.62"),
            decimal.Decimal("0"),
        ]

        write_table("//tmp/table", [{"key": encode_decimal(d, 3, 2)} for d in data])
        sort(in_="//tmp/table", out="//tmp/table", sort_by=["key"])

        actual = [decode_decimal(row["key"], 3, 2) for row in read_table("//tmp/table")]

        assert [
            decimal.Decimal("-Infinity"),
            decimal.Decimal("-6.62"),
            decimal.Decimal("0"),
            decimal.Decimal("2.71"),
            decimal.Decimal("3.14"),
            decimal.Decimal("Infinity"),
            YtNaN,
        ] == actual

    @pytest.mark.timeout(60)
    @pytest.mark.parametrize("precision", [p for p in INTERESTING_DECIMAL_PRECISION_LIST])
    @authors("ermolovd")
    def test_decimal_sort_random(self, precision):
        scale = precision / 2
        digits = "0123456789"
        rnd = random.Random()
        rnd.seed(42)

        def generate_random_decimal():
            decimal_text = (
                "".join(rnd.choice(digits) for _ in xrange(precision - scale))
                + "." + "".join(rnd.choice(digits) for _ in xrange(scale))
            )
            return decimal.Decimal(decimal_text)

        data = [generate_random_decimal() for _ in xrange(1000)]

        for d in data:
            assert d == decode_decimal(encode_decimal(d, precision, scale), precision, scale)

        create_table(
            "//tmp/table",
            schema=make_schema([{"name": "key", "type_v3": decimal_type(precision, scale)}])
        )

        write_table("//tmp/table", [{"key": encode_decimal(d, precision, scale)} for d in data])
        sort(in_="//tmp/table", out="//tmp/table", sort_by=["key"])

        actual = [decode_decimal(row["key"], precision, scale) for row in read_table("//tmp/table")]
        expected = list(sorted(data))

        if actual != expected:
            assert len(actual) == len(expected)
            for i in xrange(len(actual)):
                assert actual[i] == expected[i], "Mismatch on position {}; {} != {} ".format(i, actual[i], expected[i])

    @authors("ermolovd")
    def test_set_old_schema(self):
        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "foo",
                            "type": "int64",
                        }
                    ],
                    strict=True,
                    unique_keys=False,
                )
            },
        )

        assert get("//tmp/table/@schema/0/type_v3") == optional_type("int64")

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "foo",
                            "type": "uint8",
                            "required": True,
                        }
                    ],
                    strict=True,
                    unique_keys=False,
                )
            },
        )

        assert get("//tmp/table/@schema/0/type_v3") == "uint8"

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "foo",
                            "type": "utf8",
                            "required": False,
                        }
                    ],
                    strict=True,
                    unique_keys=False,
                )
            },
        )

        assert get("//tmp/table/@schema/0/type_v3") == optional_type("utf8")

    @authors("ermolovd")
    def test_set_new_schema(self):
        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "foo",
                            "type_v3": optional_type("int8"),
                        }
                    ],
                    strict=True,
                    unique_keys=False,
                )
            },
        )

        assert get("//tmp/table/@schema/0/type") == "int8"
        assert not get("//tmp/table/@schema/0/required")

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "foo",
                            "type_v3": "string",
                        }
                    ],
                    strict=True,
                    unique_keys=False,
                )
            },
        )

        assert get("//tmp/table/@schema/0/type") == "string"
        assert get("//tmp/table/@schema/0/required")

    @authors("ermolovd")
    def test_set_both_schemas(self):
        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={"schema": make_schema([{"name": "foo", "type": "uint32", "type_v3": "uint32"}])},
        )

        assert get("//tmp/table/@schema/0/type") == "uint32"
        assert get("//tmp/table/@schema/0/required")
        assert get("//tmp/table/@schema/0/type_v3") == "uint32"

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "foo",
                            "type": "double",
                            "required": False,
                            "type_v3": optional_type("double"),
                        }
                    ]
                )
            },
        )

        assert get("//tmp/table/@schema/0/type") == "double"
        assert not get("//tmp/table/@schema/0/required")
        assert get("//tmp/table/@schema/0/type_v3") == optional_type("double")

        create(
            "table",
            "//tmp/table",
            force=True,
            attributes={
                "schema": make_schema(
                    [
                        {
                            "name": "foo",
                            "type": "boolean",
                            "required": True,
                            "type_v3": "bool",
                        }
                    ]
                )
            },
        )

        assert get("//tmp/table/@schema/0/type") == "boolean"
        assert get("//tmp/table/@schema/0/required")
        assert get("//tmp/table/@schema/0/type_v3") == "bool"

        with raises_yt_error("Error validating column"):
            create(
                "table",
                "//tmp/table",
                force=True,
                attributes={
                    "schema": make_schema(
                        [
                            {
                                "name": "foo",
                                "type": "double",
                                "type_v3": "string",
                            }
                        ]
                    )
                },
            )

    @authors("ermolovd")
    def test_complex_types_disallowed_in_dynamic_tables(self):
        sync_create_cells(1)
        with raises_yt_error("Complex types are not allowed in dynamic tables"):
            create(
                "table",
                "//test-dynamic-table",
                attributes={
                    "schema": make_schema(
                        [
                            {
                                "name": "key",
                                "type_v3": "string",
                                "sort_order": "ascending",
                            },
                            {
                                "name": "value",
                                "type_v3": optional_type(optional_type("string")),
                            },
                        ],
                        unique_keys=True,
                    ),
                    "dynamic": True,
                },
            )

        with raises_yt_error("Complex types are not allowed in dynamic tables"):
            create(
                "table",
                "//test-dynamic-table",
                attributes={
                    "schema": make_schema(
                        [
                            {
                                "name": "key",
                                "type_v3": "string",
                                "sort_order": "ascending",
                            },
                            {
                                "name": "value",
                                "type_v3": optional_type(optional_type("string")),
                            },
                        ],
                        unique_keys=True,
                    ),
                    "dynamic": True,
                },
            )

    @authors("ermolovd")
    def test_complex_types_alter(self):
        create(
            "table",
            "//table",
            attributes={
                "schema": make_schema(
                    [
                        make_column("column", list_type("int64")),
                    ]
                )
            },
        )
        tx_write_table("//table", [{"column": []}])

        with raises_yt_error("Cannot insert a new required column"):
            alter_table(
                "//table",
                schema=make_schema(
                    [
                        make_column("column", list_type("int64")),
                        make_column("column2", list_type("int64")),
                    ]
                ),
            )

        alter_table(
            "//table",
            schema=make_schema(
                [
                    make_column("column", list_type("int64")),
                    make_column("column2", optional_type(list_type("int64"))),
                ]
            ),
        )

    @authors("ermolovd")
    def test_infer_tagged_schema(self):
        table = "//tmp/input1"
        create(
            "table",
            table,
            attributes={
                "schema": make_schema(
                    [{"name": "value", "type_v3": tagged_type("some-tag", "string")}],
                    unique_keys=False,
                    strict=True,
                )
            },
        )
        table = "//tmp/input2"
        create(
            "table",
            table,
            attributes={
                "schema": make_schema(
                    [
                        {"name": "value", "type_v3": "string"},
                    ],
                    unique_keys=False,
                    strict=True,
                )
            },
        )

        tx_write_table("//tmp/input1", [{"value": "foo"}])
        tx_write_table("//tmp/input2", [{"value": "bar"}])

        create("table", "//tmp/output")

        with raises_yt_error("tables have incompatible schemas"):
            merge(
                in_=["//tmp/input1", "//tmp/input2"],
                out="//tmp/output",
                mode="unordered",
            )

        merge(
            in_=["//tmp/input1", "//tmp/input2"],
            out="<schema=[{name=value;type_v3=utf8}]>//tmp/output",
            spec={"schema_inference_mode": "from_output"},
            mode="unordered",
        )

    @authors("ermolovd")
    def test_infer_null_void(self):
        table = "//tmp/input1"
        create(
            "table",
            table,
            attributes={
                "schema": make_schema(
                    [
                        {"name": "value", "type_v3": "void"},
                    ],
                    unique_keys=False,
                    strict=True,
                )
            },
        )
        table = "//tmp/input2"
        create(
            "table",
            table,
            attributes={
                "schema": make_schema(
                    [
                        {"name": "value", "type_v3": "null"},
                    ],
                    unique_keys=False,
                    strict=True,
                )
            },
        )

        tx_write_table("//tmp/input1", [{"value": None}])
        tx_write_table("//tmp/input2", [{"value": None}])

        create("table", "//tmp/output")

        with raises_yt_error("tables have incompatible schemas"):
            merge(
                in_=["//tmp/input1", "//tmp/input2"],
                out="//tmp/output",
                mode="unordered",
            )

        merge(
            in_=["//tmp/input1", "//tmp/input2"],
            out="<schema=[{name=value;type_v3=null}]>//tmp/output",
            spec={"schema_inference_mode": "from_output"},
            mode="unordered",
        )


class TestLogicalType(YTEnvSetup):
    USE_DYNAMIC_TABLES = True

    @authors("ermolovd")
    @pytest.mark.parametrize("table_type", ["static", "dynamic"])
    def test_logical_types(self, table_type):
        dynamic = table_type == "dynamic"
        if dynamic:
            sync_create_cells(1)

        type_tester = TypeTester(
            [
                "int8",
                "int16",
                "int32",
                "int64",
                "uint8",
                "uint16",
                "uint32",
                "uint64",
                "utf8",
                "string",
                "null",
                "date",
                "datetime",
                "timestamp",
                "interval",
                "float",
                "json",
            ],
            dynamic=dynamic,
        )

        type_tester.check_good_value("int8", 2 ** 7 - 1)
        type_tester.check_good_value("int8", 0)
        type_tester.check_good_value("int8", -(2 ** 7))
        type_tester.check_bad_value("int8", 2 ** 7)
        type_tester.check_bad_value("int8", -(2 ** 7) - 1)

        type_tester.check_good_value("int16", 2 ** 15 - 1)
        type_tester.check_good_value("int16", 0)
        type_tester.check_good_value("int16", -(2 ** 15))
        type_tester.check_bad_value("int16", 2 ** 15)
        type_tester.check_bad_value("int16", -(2 ** 15) - 1)

        type_tester.check_good_value("int32", 2 ** 31 - 1)
        type_tester.check_good_value("int32", 0)
        type_tester.check_good_value("int32", -(2 ** 31))
        type_tester.check_bad_value("int32", 2 ** 31)
        type_tester.check_bad_value("int32", -(2 ** 31) - 1)

        type_tester.check_good_value("int64", 2 ** 63 - 1)
        type_tester.check_good_value("int64", 0)
        type_tester.check_good_value("int64", -(2 ** 63))
        type_tester.check_conversion_error("int64", 2 ** 63)

        type_tester.check_good_value("uint8", 0)
        type_tester.check_good_value("uint8", 1)
        type_tester.check_good_value("uint8", 2 ** 8 - 1)
        type_tester.check_bad_value("uint8", 2 ** 8)
        type_tester.check_conversion_error("uint8", -1)

        type_tester.check_good_value("uint16", 0)
        type_tester.check_good_value("uint16", 2 ** 16 - 1)
        type_tester.check_bad_value("uint16", 2 ** 16)
        type_tester.check_conversion_error("uint16", -1)

        type_tester.check_good_value("uint32", 0)
        type_tester.check_good_value("uint32", 2 ** 32 - 1)
        type_tester.check_bad_value("uint32", 2 ** 32)
        type_tester.check_conversion_error("uint32", -1)

        type_tester.check_good_value("uint64", 0)
        type_tester.check_good_value("uint64", 2 ** 64 - 1)
        type_tester.check_conversion_error("uint64", -1)

        type_tester.check_good_value("utf8", "ff")
        type_tester.check_good_value("utf8", "ЫТЬ")
        type_tester.check_bad_value("utf8", "\xFF")
        type_tester.check_bad_value("utf8", 1)

        type_tester.check_good_value("string", "ff")
        type_tester.check_good_value("string", "ЫТЬ")
        type_tester.check_good_value("string", "\xFF")
        type_tester.check_bad_value("string", 1)

        type_tester.check_good_value("null", None)
        type_tester.check_bad_value("null", 0)
        type_tester.check_bad_value("null", False)
        type_tester.check_bad_value("null", "")

        date_upper_bound = 49673
        type_tester.check_good_value("date", 0)
        type_tester.check_good_value("date", 5)
        type_tester.check_good_value("date", date_upper_bound - 1)
        type_tester.check_conversion_error("date", -1)
        type_tester.check_bad_value("date", date_upper_bound)

        datetime_upper_bound = date_upper_bound * 86400
        type_tester.check_good_value("datetime", 0)
        type_tester.check_good_value("datetime", 5)
        type_tester.check_good_value("datetime", datetime_upper_bound - 1)
        type_tester.check_conversion_error("datetime", -1)
        type_tester.check_bad_value("datetime", datetime_upper_bound)

        timestamp_upper_bound = datetime_upper_bound * 10 ** 6
        type_tester.check_good_value("timestamp", 0)
        type_tester.check_good_value("timestamp", 5)
        type_tester.check_good_value("timestamp", timestamp_upper_bound - 1)
        type_tester.check_conversion_error("timestamp", -1)
        type_tester.check_bad_value("timestamp", timestamp_upper_bound)

        type_tester.check_good_value("interval", 0)
        type_tester.check_good_value("interval", 5)
        type_tester.check_good_value("interval", timestamp_upper_bound - 1)
        type_tester.check_good_value("interval", -timestamp_upper_bound + 1)
        type_tester.check_bad_value("interval", timestamp_upper_bound)
        type_tester.check_bad_value("interval", -timestamp_upper_bound)

        type_tester.check_good_value("float", -3.14)
        type_tester.check_good_value("float", -3.14e35)
        type_tester.check_good_value("float", 3.14e35)
        type_tester.check_bad_value("float", 3.14e135)
        type_tester.check_bad_value("float", -3.14e135)

        type_tester.check_good_value("json", "null")
        type_tester.check_good_value("json", "true")
        type_tester.check_good_value("json", "false")
        type_tester.check_good_value("json", "3.25")
        type_tester.check_good_value("json", "3.25e14")
        type_tester.check_good_value("json", '"x"')
        type_tester.check_good_value("json", '{"x": "y"}')
        type_tester.check_good_value("json", '[2, "foo", null, {}]')
        type_tester.check_bad_value("json", "without_quoutes")
        type_tester.check_bad_value("json", "False")
        type_tester.check_bad_value("json", "}{")
        type_tester.check_bad_value("json", '{3: "wrong key type"}')
        type_tester.check_bad_value("json", "Non-utf8: \xFF")

    @authors("ermolovd")
    def test_bad_alter_table(self):
        def expect_error_alter_table(schema_before, schema_after):
            remove("//test-alter-table", force=True)
            create("table", "//test-alter-table", attributes={"schema": schema_before})
            # Make table nonempty, since empty table allows any alter
            tx_write_table("//test-alter-table", [{}])
            with raises_yt_error(yt_error_codes.IncompatibleSchemas):
                alter_table("//test-alter-table", schema=schema_after)

        for (source_type, bad_destination_type_list) in [
            ("int8", ["uint64", "uint8", "string"]),
            ("int16", ["uint16", "uint16", "string", "int8"]),
            ("int32", ["uint32", "uint32", "string", "int8", "int16"]),
        ]:
            for destination_type in bad_destination_type_list:
                expect_error_alter_table(
                    [{"name": "column_name", "type": source_type}],
                    [{"name": "column_name", "type": destination_type}],
                )

    @authors("ermolovd")
    def test_logical_type_column_constrains(self):
        with raises_yt_error('Computed column "key1" type mismatch: declared type'):
            create(
                "table",
                "//test-table",
                attributes={
                    "schema": [
                        {"name": "key1", "type": "int32", "expression": "100"},
                    ]
                },
            )

        with raises_yt_error('Aggregated column "key1" is forbidden to have logical type'):
            create(
                "table",
                "//test-table",
                attributes={
                    "schema": [
                        {"name": "key1", "type": "int32", "aggregate": "sum"},
                    ]
                },
            )


class TestRequiredOption(YTEnvSetup):
    USE_DYNAMIC_TABLES = True
    NUM_SCHEDULERS = 1

    @authors("ermolovd")
    def test_required_static_tables(self):
        create(
            "table",
            "//tmp/required_table",
            attributes={
                "schema": [
                    {
                        "name": "value",
                        "type": "string",
                        "required": True,
                    }
                ],
            },
        )

        tx_write_table("//tmp/required_table", [{"value": "foo"}])
        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/required_table", [{"value": 100500}])
        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/required_table", [{"value": None}])
        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/required_table", [{}])

    @authors("ermolovd")
    def test_required_any_is_disallowed(self):
        with raises_yt_error('Column of type "any" cannot be "required"'):
            create(
                "table",
                "//tmp/required_table",
                attributes={
                    "schema": [
                        {
                            "name": "value",
                            "type": "any",
                            "required": True,
                        }
                    ],
                },
            )
        with raises_yt_error('Column of type "any" cannot be "required"'):
            create(
                "table",
                "//tmp/dynamic_required_table",
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
                        },
                    ],
                },
            )

    @authors("ermolovd")
    def test_alter_required_column(self):
        table = "//tmp/static_table"
        create(
            "table",
            table,
            attributes={
                "schema": [
                    {
                        "name": "column",
                        "type": "string",
                    }
                ],
            },
        )
        tx_write_table(table, [{"column": None}])
        with raises_yt_error(yt_error_codes.IncompatibleSchemas):
            alter_table(
                table,
                schema=[
                    {
                        "name": "column",
                        "type": "string",
                        "required": True,
                    }
                ],
            )
        tx_write_table(table, [{"column": None}])

        create(
            "table",
            table,
            force=True,
            attributes={
                "schema": [
                    {
                        "name": "column",
                        "type": "string",
                        "required": True,
                    }
                ],
            },
        )
        tx_write_table(table, [{"column": "foo"}])

        # No exception.
        alter_table(
            table,
            schema=[
                {
                    "name": "column",
                    "type": "string",
                }
            ],
        )

        create(
            "table",
            table,
            force=True,
            attributes={
                "schema": [
                    {
                        "name": "column1",
                        "type": "string",
                    }
                ],
            },
        )
        tx_write_table(table, [{"column1": "foo"}])

        with raises_yt_error("Cannot insert a new required column "):
            alter_table(
                table,
                schema=[
                    {
                        "name": "column1",
                        "type": "string",
                    },
                    {
                        "name": "column2",
                        "type": "string",
                        "required": True,
                    },
                ],
            )

    @authors("ermolovd")
    @pytest.mark.parametrize("sorted_table", [False, True])
    def test_infer_required_column(self, sorted_table):
        if sorted_table:
            schema = make_schema(
                [
                    {
                        "name": "key",
                        "type": "string",
                        "required": False,
                        "sort_order": "ascending",
                    },
                    {"name": "value", "type": "string", "required": True},
                ],
                unique_keys=False,
                strict=True,
            )
        else:
            schema = make_schema(
                [
                    {"name": "key", "type": "string", "required": False},
                    {"name": "value", "type": "string", "required": True},
                ],
                unique_keys=False,
                strict=True,
            )
        table = "//tmp/input1"
        create("table", table, attributes={"schema": schema})
        table = "//tmp/input2"
        create("table", table, attributes={"schema": schema})
        tx_write_table("//tmp/input1", [{"key": "foo", "value": "bar"}])
        tx_write_table("//tmp/input2", [{"key": "foo", "value": "baz"}])

        create("table", "//tmp/output")

        mode = "sorted" if sorted_table else "unordered"
        merge(in_=["//tmp/input1", "//tmp/input2"], out="//tmp/output", mode=mode)

        assert normalize_schema(get("//tmp/output/@schema")) == schema

    @authors("ermolovd")
    def test_infer_mixed_requiredness(self):
        table = "//tmp/input1"
        create(
            "table",
            table,
            attributes={"schema": make_schema([make_column("value", "string")], unique_keys=False, strict=True)},
        )
        table = "//tmp/input2"
        create(
            "table",
            table,
            attributes={
                "schema": make_schema(
                    [make_column("value", optional_type("string"))],
                    unique_keys=False,
                    strict=True,
                )
            },
        )

        tx_write_table("//tmp/input1", [{"value": "foo"}])
        tx_write_table("//tmp/input2", [{"value": "bar"}])

        create("table", "//tmp/output")

        with raises_yt_error("tables have incompatible schemas"):
            # Schemas are incompatible
            merge(
                in_=["//tmp/input1", "//tmp/input2"],
                out="//tmp/output",
                mode="unordered",
            )

    @authors("ifsmirnov")
    def test_required_columns_in_dynamic_tables_schema(self):
        schema = [
            {
                "name": "key_req",
                "type": "int64",
                "sort_order": "ascending",
                "required": True,
            },
            {"name": "key_opt", "type": "int64", "sort_order": "ascending"},
            {"name": "value_req", "type": "string", "required": True},
            {"name": "value_opt", "type": "string"},
        ]

        sync_create_cells(1)
        create("table", "//tmp/t", attributes={"schema": schema, "dynamic": True})

        sync_mount_table("//tmp/t")
        insert_rows(
            "//tmp/t",
            [{"key_req": 1, "key_opt": 2, "value_req": "x", "value_opt": "y"}],
        )
        sync_unmount_table("//tmp/t")

        # Required columns cannot be added
        with raises_yt_error("Cannot insert a new required column"):
            alter_table(
                "//tmp/t",
                schema=schema + [{"name": "value3_req", "type": "string", "required": True}],
            )

        # Adding non-required columns is OK
        schema += [{"name": "value3_opt", "type": "string", "required": False}]
        alter_table("//tmp/t", schema=schema)

        # Old column cannot become required
        bad_schema = [i.copy() for i in schema]
        bad_schema[3]["required"] = True
        with raises_yt_error(yt_error_codes.IncompatibleSchemas):
            alter_table("//tmp/t", schema=bad_schema)

        # Removing 'required' attribute is OK
        good_schema = [i.copy() for i in schema]
        good_schema[2]["required"] = False
        alter_table("//tmp/t", schema=good_schema)


class TestSchemaDeduplication(YTEnvSetup):
    def _get_schema(self, strict):
        return make_schema([make_column("value", "string")], unique_keys=False, strict=strict)

    @authors("ermolovd")
    def test_empty_schema(self):
        create("table", "//tmp/table")
        assert get("//tmp/table/@schema_duplicate_count") == 2

    @authors("ermolovd")
    def test_simple_schema(self):
        create("table", "//tmp/table1", attributes={"schema": self._get_schema(True)})
        create("table", "//tmp/table2", attributes={"schema": self._get_schema(True)})
        create("table", "//tmp/table3", attributes={"schema": self._get_schema(False)})

        assert get("//tmp/table1/@schema_duplicate_count") == 2
        assert get("//tmp/table2/@schema_duplicate_count") == 2
        assert get("//tmp/table3/@schema_duplicate_count") == 1

        alter_table("//tmp/table2", schema=self._get_schema(False))

        assert get("//tmp/table1/@schema_duplicate_count") == 1
        assert get("//tmp/table2/@schema_duplicate_count") == 2
        assert get("//tmp/table3/@schema_duplicate_count") == 2


class TestSchemaObjects(TestSchemaDeduplication):
    NUM_SECONDARY_MASTER_CELLS = 2

    @authors("shakurov")
    def test_schema_map(self):
        create("table", "//tmp/empty_schema_holder")
        empty_schema_0_id = get("//tmp/empty_schema_holder/@schema_id")

        create("portal_entrance", "//tmp/p1", attributes={"exit_cell_tag": 1})
        create("table", "//tmp/p1/empty_schema_holder")
        empty_schema_1_id = get("//tmp/p1/empty_schema_holder/@schema_id")

        create("portal_entrance", "//tmp/p2", attributes={"exit_cell_tag": 2})
        create("table", "//tmp/p2/empty_schema_holder")
        empty_schema_2_id = get("//tmp/p2/empty_schema_holder/@schema_id")

        assert empty_schema_0_id != empty_schema_1_id
        assert empty_schema_0_id != empty_schema_2_id
        assert empty_schema_1_id != empty_schema_2_id

        create("table", "//tmp/schema_holder1", attributes={"schema": self._get_schema(True)})
        create("table", "//tmp/schema_holder2", attributes={"schema": self._get_schema(False)})
        schema1_id = get("//tmp/schema_holder1/@schema_id")
        schema2_id = get("//tmp/schema_holder2/@schema_id")
        assert schema1_id != schema2_id

        expected_schemas = {
            empty_schema_0_id,
            empty_schema_1_id,
            empty_schema_2_id,
            schema1_id,
            schema2_id}
        assert len(expected_schemas) == 5  # Validate there're no duplicates.
        actual_schemas = {schema_id for schema_id in ls("//sys/master_table_schemas")}
        for expected_schema_id in expected_schemas:
            assert expected_schema_id in actual_schemas

        remove("//tmp/schema_holder2")
        wait(lambda: not exists("#" + schema2_id))

        expected_schemas = {
            empty_schema_0_id,
            empty_schema_1_id,
            empty_schema_2_id,
            schema1_id}
        actual_schemas = {schema_id for schema_id in ls("//sys/master_table_schemas")}
        for expected_schema_id in expected_schemas:
            assert expected_schema_id in actual_schemas

    @authors("shakurov")
    def test_schema_id(self):
        # @schema only.
        create("table", "//tmp/table1", attributes={"schema": self._get_schema(True)})

        schema = get("//tmp/table1/@schema")
        schema_id = get("//tmp/table1/@schema_id")

        # @schema_id only.
        create("table", "//tmp/table2", attributes={"schema_id": schema_id})
        assert get("//tmp/table2/@schema_id") == schema_id
        assert get("//tmp/table2/@schema") == schema

        # Both @schema and @schema_id.
        create("table", "//tmp/table3", attributes={"schema_id": schema_id, "schema": schema})
        assert get("//tmp/table3/@schema_id") == schema_id
        assert get("//tmp/table3/@schema") == schema

        # Invalid @schema_id only.
        with raises_yt_error("No such schema"):
            create("table", "//tmp/table4", attributes={"schema_id": "a-b-c-d"})

        # @schema and invalid @schema_id.
        with raises_yt_error("No such schema"):
            create("table", "//tmp/table5", attributes={"schema_id": "a-b-c-d", "schema": schema})

        other_schema = make_schema([make_column("some_column", "int8")], unique_keys=False, strict=True)

        # Hitherto-unseen @schema and a mismatching @schema_id.
        with raises_yt_error("Both \"schema\" and \"schema_id\" specified and the schemas do not match"):
            create("table", "//tmp/table6", attributes={"schema_id": schema_id, "schema": other_schema})

        create("table", "//tmp/other_schema_holder", attributes={"schema": other_schema})

        # @schema and a mismatching @schema_id.
        with raises_yt_error("Both \"schema\" and \"schema_id\" specified and they refer to different schemas"):
            create("table", "//tmp/table7", attributes={"schema_id": schema_id, "schema": other_schema})

        assert get("#" + schema_id + "/@ref_counter") == 3
        assert get("//tmp/table1/@schema_duplicate_count") == 3
        assert get("//tmp/table2/@schema_duplicate_count") == 3
        assert get("//tmp/table3/@schema_duplicate_count") == 3

    @authors("shakurov")
    def test_create_with_schema(self):
        create("table", "//tmp/table1", attributes={"schema": self._get_schema(True), "external_cell_tag": 1})
        table_id = get("//tmp/table1/@id")
        schema_id = get("//tmp/table1/@schema_id")
        external_schema_id = get("#" + table_id + "/@schema_id", driver=get_driver(1))
        assert schema_id != external_schema_id
        schema = get("//tmp/table1/@schema")
        external_schema = get("#" + table_id + "/@schema", driver=get_driver(1))
        assert schema == external_schema

    @authors("shakurov")
    def test_create_with_schema_id(self):
        # Just to have a pre-existing schema to refer to by ID.
        create("table", "//tmp/schema_holder", attributes={"schema": self._get_schema(True)})
        schema_id = get("//tmp/schema_holder/@schema_id")

        create("table", "//tmp/table1", attributes={"schema_id": schema_id, "external_cell_tag": 1})
        table_id = get("//tmp/table1/@id")
        assert get("//tmp/table1/@schema_id") == schema_id
        external_schema_id = get("#" + table_id + "/@schema_id", driver=get_driver(1))
        assert schema_id != external_schema_id
        schema = get("//tmp/table1/@schema")
        external_schema = get("#" + table_id + "/@schema", driver=get_driver(1))
        assert schema == external_schema

    @authors("shakurov")
    @pytest.mark.parametrize("cross_shard", [False, True])
    def test_copy_with_schema(self, cross_shard):
        create("table", "//tmp/table1", attributes={"schema": self._get_schema(True), "external_cell_tag": 1})
        if cross_shard:
            create("portal_entrance", "//tmp/d", attributes={"exit_cell_tag": 2})
        else:
            create("map_node", "//tmp/d")

        copy("//tmp/table1", "//tmp/d/table1_copy")

        src_table_id = get("//tmp/table1/@id")
        dst_table_id = get("//tmp/d/table1_copy/@id")

        src_schema = get("//tmp/table1/@schema")
        src_schema_id = get("//tmp/table1/@schema_id")
        external_src_schema = get("#" + src_table_id + "/@schema", driver=get_driver(1))
        external_src_schema_id = get("#" + src_table_id + "/@schema_id", driver=get_driver(1))
        dst_schema = get("//tmp/d/table1_copy/@schema")
        dst_schema_id = get("//tmp/d/table1_copy/@schema_id")
        external_dst_schema = get("#" + dst_table_id + "/@schema", driver=get_driver(1))
        external_dst_schema_id = get("#" + dst_table_id + "/@schema_id", driver=get_driver(1))

        # All schemas are identical.
        assert src_schema == external_src_schema
        assert src_schema == dst_schema
        assert src_schema == external_dst_schema

        # External schema is always shared.
        assert external_src_schema_id == external_dst_schema_id
        # Native schemas differ when on different cellls.
        if cross_shard:
            assert src_schema_id != dst_schema_id
        else:
            assert src_schema_id == dst_schema_id
        # Everything else always differs.
        assert src_schema_id != external_src_schema_id
        assert src_schema_id != external_dst_schema_id
        assert external_src_schema_id != dst_schema_id
        assert dst_schema_id != external_dst_schema_id


class TestSchemaValidation(YTEnvSetup):
    @authors("ermolovd")
    def test_schema_complexity(self):
        def make_schema(size):
            return [{"name": "column{}".format(i), "type": "int64"} for i in range(size)]

        def make_row(size):
            return {"column{}".format(i): i for i in range(size)}

        bad_size = 32 * 1024
        with raises_yt_error("Table schema is too complex"):
            create(
                "table",
                "//tmp/bad-schema-1",
                attributes={"schema": make_schema(bad_size)},
            )

        create("table", "//tmp/bad-schema-2")
        with raises_yt_error("Too many columns in row"):
            tx_write_table("//tmp/bad-schema-2", [make_row(bad_size)])

        ok_size = bad_size - 1
        create("table", "//tmp/ok-schema", attributes={"schema": make_schema(ok_size)})
        tx_write_table("//tmp/ok-schema", [make_row(ok_size)])

    @authors("levysotsky")
    def test_is_comparable(self):
        def check_comparable(type):
            schema = [{"name": "column", "type_v3": type, "sort_order": "ascending"}]
            create("table", "//tmp/t", attributes={"schema": schema})
            remove("//tmp/t")

        def check_not_comparable(type):
            with raises_yt_error("Key column cannot be of"):
                check_comparable(type)

        for t in [
            "int8",
            "int16",
            "int32",
            "int64",
            "uint8",
            "uint16",
            "uint32",
            "uint64",
            "utf8",
            "string",
            "null",
            "date",
            "datetime",
            "timestamp",
            "interval",
            optional_type("yson"),
            "float",
            list_type(optional_type("int32")),
            tuple_type([list_type("date"), optional_type("datetime")]),
        ]:
            check_comparable(t)

        for t in [
            "json",
            struct_type(
                [
                    ("a", "int64"),
                    ("b", "string"),
                ]
            ),
            tuple_type(["json", "int8"]),
            tuple_type([optional_type("yson")]),
        ]:
            check_not_comparable(t)


@authors("ermolovd")
class TestErrorCodes(YTEnvSetup):
    USE_DYNAMIC_TABLES = True

    def test_YT_11522_missing_column(self):
        schema = [
            {
                "name": "foo",
                "type": "int64",
                "sort_order": "ascending",
                "required": True,
            },
            {"name": "bar", "type": "int64"},
        ]

        sync_create_cells(1)
        create("table", "//tmp/t", attributes={"schema": schema, "dynamic": True})

        sync_mount_table("//tmp/t")
        with raises_yt_error(yt_error_codes.SchemaViolation):
            insert_rows("//tmp/t", [{"baz": 1}])
        sync_unmount_table("//tmp/t")

    def test_YT_11522_convesion_error(self):
        schema = [{"name": "foo", "type": "uint64"}]

        create("table", "//tmp/t", attributes={"schema": schema})

        with raises_yt_error(yt_error_codes.SchemaViolation):
            tx_write_table("//tmp/t", [{"foo": -1}])


@authors("ermolovd")
class TestAlterTable(YTEnvSetup):
    USE_DYNAMIC_TABLES = True

    _TABLE_PATH = "//tmp/test-alter-table"

    def get_default_value_for_type(self, type_v3):
        if isinstance(type_v3, str):
            if type_v3.startswith("int"):
                return 0
            elif type_v3.startswith("uint"):
                return yson.YsonUint64(0)
            elif type_v3 == "bool":
                return False
            elif type_v3 in ["string", "utf8"]:
                return ""
            raise ValueError("Type {} is not supported".format(type_v3))
        type_name = type_v3["type_name"]
        if type_name == "optional":
            return None
        elif type_name == "list":
            return []
        elif type_name == "tuple":
            return [
                self.get_default_value_for_type(t["type"]) for t in type_v3["elements"]
            ]
        elif type_name == "struct":
            return {
                m["name"]: self.get_default_value_for_type(m["type"]) for m in type_v3["members"]
            }
        elif type_name == "variant":
            if "members" in type_v3:
                member = type_v3["members"][0]
                return [member["name"], self.get_default_value_for_type(member["type"])]
            elif "elements" in type_v3:
                return [0, self.get_default_value_for_type(type_v3["elements"][0]["type"])]
            raise ValueError("Unknown kind of variant: {}".format(type_v3))
        elif type_name == "dict":
            return []
        elif type_name == "tagged":
            return self.get_default_value_for_type(type_v3["item"])
        raise ValueError("Type {} is not supported".format(type_name))

    def get_default_row(self, schema):
        return {
            column["name"]: self.get_default_value_for_type(column["type_v3"])
            for column in schema
        }

    def _create_test_schema_with_type(self, type_v3):
        # Schema is compatible with both static and dynamic tables
        return make_schema([
            make_column("key", "int64", sort_order="ascending"),
            make_column("value", type_v3),
        ], unique_keys=True, strict=True)

    def prepare_table(self, schema, dynamic=False):
        create("table", self._TABLE_PATH, force=True, attributes={
            "schema": schema,
        })
        write_table(self._TABLE_PATH, [self.get_default_row(schema)])
        if dynamic:
            alter_table(self._TABLE_PATH, dynamic=True)

    def check_both_ways_alter_type(self, old_type_v3, new_type_v3, dynamic=False):
        old_schema = self._create_test_schema_with_type(old_type_v3)
        new_schema = self._create_test_schema_with_type(new_type_v3)
        self.prepare_table(old_schema, dynamic=dynamic)

        alter_table(self._TABLE_PATH, schema=new_schema)
        alter_table(self._TABLE_PATH, schema=old_schema)

    def check_one_way_alter_type(self, old_type_v3, new_type_v3, dynamic=False):
        """
        Check that we can alter column type from old_type_v3 to new_type_v3 but cannot alter it back.
        """
        old_schema = self._create_test_schema_with_type(old_type_v3)
        new_schema = self._create_test_schema_with_type(new_type_v3)
        self.prepare_table(old_schema, dynamic=dynamic)

        alter_table(self._TABLE_PATH, schema=new_schema)
        with raises_yt_error(yt_error_codes.IncompatibleSchemas):
            alter_table(self._TABLE_PATH, schema=old_schema)

    def check_bad_alter_type(self, old_type_v3, new_type_v3, dynamic=False):
        """
        Check that we can alter column type from old_type_v3 to new_type_v3 but cannot alter it back.
        """
        old_schema = self._create_test_schema_with_type(old_type_v3)
        new_schema = self._create_test_schema_with_type(new_type_v3)
        self.prepare_table(old_schema, dynamic=dynamic)

        with raises_yt_error(yt_error_codes.IncompatibleSchemas):
            alter_table(self._TABLE_PATH, schema=new_schema)

    def check_bad_both_way_alter_type(self, lhs_type_v3, rhs_type_v3, dynamic=False):
        self.check_bad_alter_type(lhs_type_v3, rhs_type_v3, dynamic)
        self.check_bad_alter_type(rhs_type_v3, lhs_type_v3, dynamic)

    @pytest.mark.parametrize("dynamic", [False, True])
    def test_alter_simple_types(self, dynamic):
        self.check_one_way_alter_type("int8", "int16", dynamic=dynamic)
        self.check_one_way_alter_type("int8", "int32", dynamic=dynamic)
        self.check_one_way_alter_type("int8", "int64", dynamic=dynamic)

        self.check_one_way_alter_type("uint8", "uint16", dynamic=dynamic)
        self.check_one_way_alter_type("uint8", "uint32", dynamic=dynamic)
        self.check_one_way_alter_type("uint8", "uint64", dynamic=dynamic)

        self.check_one_way_alter_type("utf8", "string", dynamic=dynamic)

        self.check_one_way_alter_type("int64", optional_type("int64"), dynamic=dynamic)
        self.check_one_way_alter_type("int8", optional_type("int64"), dynamic=dynamic)

        self.check_bad_both_way_alter_type("int64", optional_type("yson"), dynamic=dynamic)
        self.check_bad_both_way_alter_type("uint8", "int64", dynamic=dynamic)

    def test_alter_composite_types(self):
        # List
        self.check_one_way_alter_type(
            list_type("int64"),
            optional_type(list_type("int64")))
        self.check_one_way_alter_type(
            list_type("int64"),
            list_type(optional_type("int64")))

        self.check_bad_both_way_alter_type(
            list_type("int64"),
            optional_type(optional_type(list_type("int64"))))
        self.check_bad_both_way_alter_type(
            list_type("int64"),
            optional_type("yson"))
        self.check_bad_both_way_alter_type(
            optional_type("yson"),
            list_type("int64"))

        # Tuple
        self.check_one_way_alter_type(
            tuple_type(["int32"]),
            tuple_type(["int64"]))
        self.check_one_way_alter_type(
            tuple_type(["int64"]),
            tuple_type([optional_type("int64")]))
        self.check_one_way_alter_type(
            tuple_type(["int32"]),
            tuple_type([optional_type("int64")]))
        self.check_bad_both_way_alter_type(
            tuple_type(["int64"]),
            tuple_type(["int64", "int64"]))
        self.check_bad_both_way_alter_type(
            tuple_type(["int64"]),
            optional_type("yson"))
        self.check_bad_both_way_alter_type(
            optional_type("yson"),
            tuple_type(["int64"]))

        # Struct
        self.check_one_way_alter_type(
            struct_type([("a", "int32")]),
            struct_type([("a", "int64")]))
        self.check_one_way_alter_type(
            struct_type([("a", "int64")]),
            struct_type([("a", optional_type("int64"))]))
        self.check_one_way_alter_type(
            struct_type([("a", "int32")]),
            struct_type([("a", optional_type("int64"))]))
        self.check_bad_both_way_alter_type(
            struct_type([("a", "int64")]),
            struct_type([("a", optional_type(optional_type("int64")))]))

        self.check_one_way_alter_type(
            struct_type([("a", "int64")]),
            struct_type([("a", "int64"), ("b", optional_type("int64"))]))
        self.check_one_way_alter_type(
            struct_type([("a", "int64")]),
            struct_type([("a", "int64"), ("b", optional_type(optional_type("int64")))]))
        self.check_bad_both_way_alter_type(
            struct_type([("a", "int64")]),
            struct_type([("a", "int64"), ("b", "int64")]))
        self.check_bad_both_way_alter_type(
            struct_type([("a", "int64")]),
            struct_type([("b", optional_type("int64")), ("a", "int64")]))

        self.check_bad_both_way_alter_type(
            struct_type([("a", "int64")]),
            optional_type("yson"))
        self.check_bad_both_way_alter_type(
            optional_type("yson"),
            struct_type([("a", "int64")]))

        # Variant over tuple
        self.check_one_way_alter_type(
            variant_tuple_type(["int32"]),
            variant_tuple_type(["int64"]))
        self.check_one_way_alter_type(
            variant_tuple_type(["int64"]),
            variant_tuple_type([optional_type("int64")]))
        self.check_one_way_alter_type(
            variant_tuple_type(["int32"]),
            variant_tuple_type([optional_type("int64")]))
        self.check_one_way_alter_type(
            variant_tuple_type(["int64"]),
            variant_tuple_type(["int64", "int64"]))
        self.check_one_way_alter_type(
            variant_tuple_type(["int64"]),
            variant_tuple_type(["int64", optional_type("int64")]))
        self.check_bad_both_way_alter_type(
            variant_tuple_type(["int64"]),
            optional_type("yson"))

        # Variant over struct
        self.check_one_way_alter_type(
            variant_struct_type([("a", "int32")]),
            variant_struct_type([("a", "int64")]))
        self.check_one_way_alter_type(
            variant_struct_type([("a", "int64")]),
            variant_struct_type([("a", optional_type("int64"))]))
        self.check_one_way_alter_type(
            variant_struct_type([("a", "int32")]),
            variant_struct_type([("a", optional_type("int64"))]))
        self.check_bad_both_way_alter_type(
            variant_struct_type([("a", "int64")]),
            variant_struct_type([("a", optional_type(optional_type("int64")))]))

        self.check_one_way_alter_type(
            variant_struct_type([("a", "int64")]),
            variant_struct_type([("a", "int64"), ("b", optional_type("int64"))]))
        self.check_one_way_alter_type(
            variant_struct_type([("a", "int64")]),
            variant_struct_type([("a", "int64"), ("b", optional_type(optional_type("int64")))]))
        self.check_one_way_alter_type(
            variant_struct_type([("a", "int64")]),
            variant_struct_type([("a", "int64"), ("b", "int64")]))
        self.check_bad_both_way_alter_type(
            variant_struct_type([("a", "int64")]),
            variant_struct_type([("b", optional_type("int64")), ("a", "int64")]))
        self.check_bad_both_way_alter_type(
            variant_struct_type([("a", "int64")]),
            optional_type("yson"))

        # Dict
        self.check_one_way_alter_type(
            dict_type("utf8", "int8"),
            dict_type("utf8", optional_type("int8")))
        self.check_one_way_alter_type(
            dict_type("utf8", "int8"),
            dict_type(optional_type("utf8"), "int8"))
        self.check_one_way_alter_type(
            dict_type("utf8", "int8"),
            dict_type(optional_type("string"), "int8"))
        self.check_one_way_alter_type(
            dict_type("utf8", "int8"),
            dict_type("string", optional_type("int64")))

        self.check_bad_both_way_alter_type(
            dict_type("utf8", "int8"),
            optional_type("yson"))
        self.check_bad_both_way_alter_type(
            dict_type("utf8", "uint8"),
            dict_type("utf8", "int64"))
        self.check_bad_both_way_alter_type(
            dict_type("utf8", "uint8"),
            dict_type("int8", "uint8"))

        # Tagged
        self.check_both_ways_alter_type(
            optional_type("int8"),
            tagged_type("foo", optional_type("int8")))
        self.check_both_ways_alter_type(
            optional_type("int8"),
            optional_type(tagged_type("foo", "int8")))
        self.check_both_ways_alter_type(
            optional_type("int8"),
            tagged_type("bar", optional_type(tagged_type("foo", "int8"))))
        self.check_both_ways_alter_type(
            tagged_type("qux", optional_type("int8")),
            tagged_type("bar", optional_type(tagged_type("foo", "int8"))))
