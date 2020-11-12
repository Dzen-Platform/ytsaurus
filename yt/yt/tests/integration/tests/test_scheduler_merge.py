import pytest
import yt.yson as yson

from yt_env_setup import YTEnvSetup, parametrize_external
from yt_commands import *

from yt.environment.helpers import assert_items_equal
from time import sleep

##################################################################


class TestSchedulerMergeCommands(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "operations_update_period": 10,
            "running_jobs_update_period": 10,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10,
            "max_chunks_per_fetch": 10,
            "sorted_merge_operation_options": {
                "job_splitter": {
                    "min_job_time": 3000,
                    "min_total_data_size": 1024,
                    "update_period": 100,
                    "candidate_percentile": 0.8,
                    "max_jobs_per_split": 3,
                },
            },
            "ordered_merge_operation_options": {
                "job_splitter": {
                    "min_job_time": 3000,
                    "min_total_data_size": 1024,
                    "update_period": 100,
                    "candidate_percentile": 0.8,
                    "max_jobs_per_split": 3,
                },
            },
        }
    }

    DELTA_NODE_CONFIG = {"scheduler_connector": {"heartbeat_period": 100}}  # 100 msec

    def _prepare_tables(self):
        t1 = "//tmp/t1"
        create("table", t1)
        v1 = [{"key" + str(i): "value" + str(i)} for i in xrange(3)]
        for v in v1:
            write_table("<append=true>" + t1, v)

        t2 = "//tmp/t2"
        create("table", t2)
        v2 = [{"another_key" + str(i): "another_value" + str(i)} for i in xrange(4)]
        for v in v2:
            write_table("<append=true>" + t2, v)

        self.t1 = t1
        self.t2 = t2

        self.v1 = v1
        self.v2 = v2

        create("table", "//tmp/t_out")

    def _create_simple_dynamic_table(self, path, **attributes):
        if "schema" not in attributes:
            attributes.update(
                {
                    "schema": [
                        {"name": "key", "type": "int64", "sort_order": "ascending"},
                        {"name": "value", "type": "string"},
                    ]
                }
            )
        create_dynamic_table(path, **attributes)

    # usual cases
    @authors("panin", "ignat")
    def test_unordered(self):
        self._prepare_tables()

        merge(mode="unordered", in_=[self.t1, self.t2], out="//tmp/t_out")

        assert_items_equal(read_table("//tmp/t_out"), self.v1 + self.v2)
        assert get("//tmp/t_out/@chunk_count") == 7

    @authors("panin", "ignat")
    def test_unordered_combine(self):
        self._prepare_tables()

        merge(
            combine_chunks=True,
            mode="unordered",
            in_=[self.t1, self.t2],
            out="//tmp/t_out",
        )

        assert_items_equal(read_table("//tmp/t_out"), self.v1 + self.v2)
        assert get("//tmp/t_out/@chunk_count") == 1

    @authors("klyachin")
    def test_unordered_with_mixed_chunks(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"a": 4}, {"a": 5}, {"a": 6}])
        write_table("//tmp/t2", [{"a": 7}, {"a": 8}, {"a": 9}])
        write_table("//tmp/t3", [{"a": 1}, {"a": 2}, {"a": 3}])

        create("table", "//tmp/t_out")
        merge(
            mode="unordered",
            in_=["//tmp/t1", "//tmp/t2[:#2]", "//tmp/t3[#1:]"],
            out="//tmp/t_out",
            spec={"data_size_per_job": 1000},
        )

        assert get("//tmp/t_out/@row_count") == 7
        assert get("//tmp/t_out/@chunk_count") == 2
        assert sorted(read_table("//tmp/t_out")) == [{"a": i} for i in range(2, 9)]

    @authors("dakovalkov")
    @pytest.mark.parametrize("merge_mode", ["unordered", "ordered", "sorted"])
    def test_rename_columns(self, merge_mode):
        create(
            "table",
            "//tmp/t1",
            attributes={"schema": [{"name": "a", "type": "int64", "sort_order": "ascending"}]},
        )
        create(
            "table",
            "//tmp/t2",
            attributes={"schema": [{"name": "a2", "type": "int64", "sort_order": "ascending"}]},
        )
        write_table("//tmp/t1", [{"a": 1}, {"a": 2}])
        write_table("//tmp/t2", [{"a2": 3}, {"a2": 4}])

        create("table", "//tmp/t_out")
        merge(
            mode=merge_mode,
            in_=["//tmp/t1", "<rename_columns={a2=a}>//tmp/t2"],
            out="//tmp/t_out",
        )

        assert sorted(read_table("//tmp/t_out")) == [
            {"a": 1},
            {"a": 2},
            {"a": 3},
            {"a": 4},
        ]

    @authors("panin", "ignat")
    def test_ordered(self):
        self._prepare_tables()

        merge(mode="ordered", in_=[self.t1, self.t2], out="//tmp/t_out")

        assert read_table("//tmp/t_out") == self.v1 + self.v2
        assert get("//tmp/t_out/@chunk_count") == 7

    @authors("panin", "ignat")
    def test_ordered_combine(self):
        self._prepare_tables()

        merge(
            combine_chunks=True,
            mode="ordered",
            in_=[self.t1, self.t2],
            out="//tmp/t_out",
        )

        assert read_table("//tmp/t_out") == self.v1 + self.v2
        assert get("//tmp/t_out/@chunk_count") == 1

    @authors("ignat")
    def test_sorted(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        write_table("//tmp/t1", [{"a": 1}, {"a": 10}, {"a": 100}], sorted_by="a")
        write_table("//tmp/t2", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(mode="sorted", in_=["//tmp/t1", "//tmp/t2"], out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [
            {"a": 1},
            {"a": 2},
            {"a": 3},
            {"a": 10},
            {"a": 15},
            {"a": 100},
        ]
        assert (
            get("//tmp/t_out/@chunk_count") == 1
        )  # resulting number of chunks is always equal to 1 (as long as they are small)
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["a"]

    @authors("dakovalkov")
    def test_sorted_different_types(self):
        create(
            "table",
            "//tmp/t1",
            attributes={"schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]},
        )
        create(
            "table",
            "//tmp/t2",
            attributes={"schema": [{"name": "key", "type": "string", "sort_order": "ascending"}]},
        )
        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 1}])
        write_table("//tmp/t2", [{"key": "1"}])

        with pytest.raises(YtError):
            merge(mode="sorted", in_=["//tmp/t1", "//tmp/t2"], out="//tmp/out")

    @authors("psushin")
    def test_sorted_column_filter(self):
        create("table", "//tmp/t")
        write_table(
            "//tmp/t",
            [{"a": 1, "b": 3}, {"a": 10, "b": 2}, {"a": 100, "b": 1}],
            sorted_by="a",
        )

        create("table", "//tmp/t_out")
        with pytest.raises(YtError):
            merge(mode="sorted", in_=["<columns=[b]>//tmp/t"], out="//tmp/t_out")

    @authors("klyachin")
    def test_sorted_merge_result_is_sorted(self):
        create("table", "//tmp/t1")

        count = 100
        write_table(
            "<append=true>//tmp/t1",
            [
                {
                    "key": "%05d" % (i if i < count / 2 else count / 2),
                    "value": "%05d" % i,
                }
                for i in range(count)
            ],
            sorted_by="key",
            table_writer={"block_size": 1024},
        )
        write_table(
            "<append=true>//tmp/t1",
            [{"key": "%05d" % (count / 2), "value": "%05d" % (i + count)} for i in range(count)],
            sorted_by="key",
            table_writer={"block_size": 1024},
        )
        write_table(
            "<append=true>//tmp/t1",
            [{"key": "%05d" % (count / 2), "value": "%05d" % (i + 2 * count)} for i in range(count)],
            sorted_by="key",
            table_writer={"block_size": 1024},
        )
        write_table(
            "<append=true>//tmp/t1",
            [
                {
                    "key": "%05d" % (count / 2 if i < count / 2 else i),
                    "value": "%05d" % (i + 3 * count),
                }
                for i in range(count)
            ],
            sorted_by="key",
            table_writer={"block_size": 1024},
        )

        create("table", "//tmp/t_out")
        merge(
            mode="sorted",
            in_=["//tmp/t1"],
            out="//tmp/t_out",
            spec={"data_size_per_job": 100},
        )

        result = read_table("//tmp/t_out")
        assert len(result) == 4 * count
        for i in range(len(result) - 1):
            assert result[i]["key"] <= result[i + 1]["key"]

    @authors("psushin", "ignat")
    def test_sorted_trivial(self):
        create("table", "//tmp/t1")

        write_table("//tmp/t1", [{"a": 1}, {"a": 10}, {"a": 100}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(combine_chunks=True, mode="sorted", in_=["//tmp/t1"], out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [{"a": 1}, {"a": 10}, {"a": 100}]
        assert (
            get("//tmp/t_out/@chunk_count") == 1
        )  # resulting number of chunks is always equal to 1 (as long as they are small)
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["a"]

    @authors("monster")
    def test_append_not_sorted(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_out", [{"a": 1}, {"a": 2}, {"a": 3}], sorted_by="a")
        write_table("//tmp/t_in", [{"a": 0}])

        merge(mode="unordered", in_=["//tmp/t_in"], out="<append=true>//tmp/t_out")

        assert not get("//tmp/t_out/@sorted")

    @authors("ignat")
    def test_sorted_with_same_chunks(self):
        t1 = "//tmp/t1"
        t2 = "//tmp/t2"
        v = [{"key1": "value1"}]

        create("table", t1)
        write_table(t1, v[0])
        sort(in_=t1, out=t1, sort_by="key1")
        copy(t1, t2)

        create("table", "//tmp/t_out")
        merge(mode="sorted", in_=[t1, t2], out="//tmp/t_out")
        assert_items_equal(read_table("//tmp/t_out"), v + v)

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["key1"]

    @authors("ignat")
    def test_sorted_combine(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        write_table("//tmp/t1", [{"a": 1}, {"a": 10}, {"a": 100}], sorted_by="a")
        write_table("//tmp/t2", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(
            combine_chunks=True,
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2"],
            out="//tmp/t_out",
        )

        assert read_table("//tmp/t_out") == [
            {"a": 1},
            {"a": 2},
            {"a": 3},
            {"a": 10},
            {"a": 15},
            {"a": 100},
        ]
        assert get("//tmp/t_out/@chunk_count") == 1
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["a"]

    @authors("dakovalkov")
    def test_sorted_row_count_limit(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"k": "a", "s": 0}, {"k": "b", "s": 1}], sorted_by=["k", "s"])
        write_table("//tmp/t2", [{"k": "b", "s": 2}, {"k": "c", "s": 0}], sorted_by=["k", "s"])
        write_table("//tmp/t3", [{"k": "b", "s": 1}, {"k": "b", "s": 2}], sorted_by=["k", "s"])

        create("table", "//tmp/out")
        merge(
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
            out="<row_count_limit=2>//tmp/out",
            merge_by=["k", "s"],
        )

        assert read_table("//tmp/out") == [
            {"k": "a", "s": 0},
            {"k": "b", "s": 1},
            {"k": "b", "s": 1},
            {"k": "b", "s": 2},
            {"k": "b", "s": 2},
            {"k": "c", "s": 0},
        ]

    @authors("dakovalkov")
    def test_sorted_row_count_limit_2(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"k": "a", "s": 0}, {"k": "b", "s": 1}], sorted_by=["k", "s"])
        write_table("//tmp/t2", [{"k": "b", "s": 3}, {"k": "c", "s": 0}], sorted_by=["k", "s"])
        write_table("//tmp/t3", [{"k": "b", "s": 2}, {"k": "b", "s": 4}], sorted_by=["k", "s"])

        create("table", "//tmp/out")
        merge(
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
            out="<row_count_limit=2>//tmp/out",
            merge_by=["k", "s"],
        )

        assert read_table("//tmp/out") == [{"k": "a", "s": 0}, {"k": "b", "s": 1}]

    # TODO(max42): eventually remove this test as it duplicates unittests TSortedChunkPoolTest/SortedMergeTeleport*.
    @authors("ignat")
    def test_sorted_passthrough(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"k": "a", "s": 0}, {"k": "b", "s": 1}], sorted_by=["k", "s"])
        write_table("//tmp/t2", [{"k": "b", "s": 2}, {"k": "c", "s": 0}], sorted_by=["k", "s"])
        write_table("//tmp/t3", [{"k": "b", "s": 0}, {"k": "b", "s": 3}], sorted_by=["k", "s"])

        create("table", "//tmp/t_out")
        merge(
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2", "//tmp/t3", "//tmp/t2[(b, 3) : (b, 7)]"],
            out="//tmp/t_out",
            merge_by="k",
        )

        res = read_table("//tmp/t_out")
        expected = [
            {"k": "a", "s": 0},
            {"k": "b", "s": 1},
            {"k": "b", "s": 0},
            {"k": "b", "s": 3},
            {"k": "b", "s": 2},
            {"k": "c", "s": 0},
        ]

        assert_items_equal(res, expected)

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["k"]

        merge(
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
            out="//tmp/t_out",
            merge_by="k",
        )

        res = read_table("//tmp/t_out")
        assert_items_equal(res, expected)

        assert get("//tmp/t_out/@chunk_count") == 3
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["k"]

        merge(
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
            out="//tmp/t_out",
            merge_by=["k", "s"],
        )

        res = read_table("//tmp/t_out")
        expected = [
            {"k": "a", "s": 0},
            {"k": "b", "s": 0},
            {"k": "b", "s": 1},
            {"k": "b", "s": 2},
            {"k": "b", "s": 3},
            {"k": "c", "s": 0},
        ]

        for i, j in zip(res, expected):
            assert i == j

        assert get("//tmp/t_out/@chunk_count") == 1
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["k", "s"]

    @authors("ignat")
    def test_sorted_with_maniacs(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"a": 3}, {"a": 3}, {"a": 3}], sorted_by="a")
        write_table("//tmp/t2", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")
        write_table("//tmp/t3", [{"a": 1}, {"a": 3}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(
            combine_chunks=True,
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
            out="//tmp/t_out",
            spec={"data_size_per_job": 1},
        )

        assert read_table("//tmp/t_out") == [
            {"a": 1},
            {"a": 2},
            {"a": 3},
            {"a": 3},
            {"a": 3},
            {"a": 3},
            {"a": 3},
            {"a": 15},
        ]
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["a"]

    @authors("psushin")
    def test_sorted_with_row_limits(self):
        create("table", "//tmp/t1")

        write_table("//tmp/t1", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(combine_chunks=False, mode="sorted", in_="//tmp/t1[:#2]", out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [{"a": 2}, {"a": 3}]
        assert get("//tmp/t_out/@chunk_count") == 1

    @authors("ignat")
    def test_sorted_by(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        a1 = {"a": 1, "b": 20}
        a2 = {"a": 10, "b": 1}
        a3 = {"a": 10, "b": 2}

        b1 = {"a": 2, "c": 10}
        b2 = {"a": 10, "c": 0}
        b3 = {"a": 15, "c": 5}

        write_table("//tmp/t1", [a1, a2, a3], sorted_by=["a", "b"])
        write_table("//tmp/t2", [b1, b2, b3], sorted_by=["a", "c"])

        create("table", "//tmp/t_out")

        # error when sorted_by of input tables are different and merge_by is not set
        with pytest.raises(YtError):
            merge(mode="sorted", in_=["//tmp/t1", "//tmp/t2"], out="//tmp/t_out")

        # now merge_by is set
        merge(mode="sorted", in_=["//tmp/t1", "//tmp/t2"], out="//tmp/t_out", merge_by="a")

        result = read_table("//tmp/t_out")
        assert result[:2] == [a1, b1]
        assert_items_equal(result[2:5], [a2, a3, b2])
        assert result[5] == b3

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["a"]

    @authors("babenko")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_sorted_unique_simple(self, optimize_for):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")
        create(
            "table",
            "//tmp/t_out",
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {"name": "a", "type": "int64", "sort_order": "ascending"},
                        {"name": "b", "type": "int64"},
                    ],
                    unique_keys=True,
                ),
            },
        )

        a1 = {"a": 1, "b": 1}
        a2 = {"a": 2, "b": 2}
        a3 = {"a": 3, "b": 3}

        write_table("//tmp/t1", [a1, a2], sorted_by=["a", "b"])
        write_table("//tmp/t2", [a3], sorted_by=["a"])
        write_table("//tmp/t3", [a3, a3], sorted_by=["a", "b"])

        with pytest.raises(YtError):
            merge(
                mode="sorted",
                in_="//tmp/t3",
                out="//tmp/t_out",
                merge_by="a",
                spec={"schema_inference_mode": "from_output"},
            )

        merge(
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2"],
            out="//tmp/t_out",
            merge_by="a",
            spec={"schema_inference_mode": "from_output"},
        )

        result = read_table("//tmp/t_out")
        assert result == [a1, a2, a3]

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["a"]
        assert get("//tmp/t_out/@schema/@unique_keys")

    @authors("psushin")
    def test_sorted_unique_teleport(self):
        create(
            "table",
            "//tmp/t1",
            attributes={
                "schema": make_schema(
                    [
                        {"name": "a", "type": "int64", "sort_order": "ascending"},
                        {"name": "b", "type": "int64"},
                    ],
                    unique_keys=True,
                )
            },
        )
        create(
            "table",
            "//tmp/t_out",
            attributes={
                "schema": make_schema(
                    [
                        {"name": "a", "type": "int64", "sort_order": "ascending"},
                        {"name": "b", "type": "int64"},
                    ],
                    unique_keys=True,
                )
            },
        )

        a1 = {"a": 1, "b": 1}
        a2 = {"a": 2, "b": 2}

        write_table("//tmp/t1", [a1, a2])

        merge(mode="sorted", in_="//tmp/t1", out="//tmp/t_out", merge_by="a")

        assert read_table("//tmp/t_out") == [a1, a2]
        assert get("//tmp/t_out/@schema/@unique_keys")

    @authors("babenko")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_sorted_unique_with_wider_key_columns(self, optimize_for):
        create("table", "//tmp/t1")
        create(
            "table",
            "//tmp/t_out",
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {"name": "key1", "type": "int64", "sort_order": "ascending"},
                        {"name": "key2", "type": "int64", "sort_order": "ascending"},
                    ],
                    unique_keys=True,
                ),
            },
        )

        write_table(
            "//tmp/t1",
            [{"key1": 1, "key2": 1}, {"key1": 1, "key2": 2}],
            sorted_by=["key1", "key2"],
        )

        with pytest.raises(YtError):
            merge(
                mode="sorted",
                in_="//tmp/t1",
                out="//tmp/t_out",
                merge_by="key1",
                spec={"schema_inference_mode": "from_output"},
            )

        merge(
            mode="sorted",
            in_="//tmp/t1",
            out="//tmp/t_out",
            merge_by=["key1", "key2"],
            spec={"schema_inference_mode": "from_output"},
        )

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["key1", "key2"]
        assert get("//tmp/t_out/@schema/@unique_keys")

    @authors("ignat")
    def test_empty_in_ordered(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t_out")

        v = {"foo": "bar"}
        write_table("//tmp/t1", v)

        merge(mode="ordered", in_=["//tmp/t1", "//tmp/t2"], out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [v]

    @authors("psushin")
    def test_empty_in_sorted(self):
        create(
            "table",
            "//tmp/t1",
            attributes={"schema": [{"name": "a", "type": "int64", "sort_order": "ascending"}]},
        )
        create("table", "//tmp/t_out")

        merge(mode="sorted", in_="//tmp/t1", out="//tmp/t_out")

        assert read_table("//tmp/t_out") == []

    @authors("ignat")
    def test_non_empty_out(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t_out")

        v1 = {"value": 1}
        v2 = {"value": 2}
        v3 = {"value": 3}

        write_table("//tmp/t1", v1)
        write_table("//tmp/t2", v2)
        write_table("//tmp/t_out", v3)

        merge(mode="ordered", in_=["//tmp/t1", "//tmp/t2"], out="<append=true>//tmp/t_out")

        assert read_table("//tmp/t_out") == [v3, v1, v2]

    @authors("panin", "ignat")
    def test_multiple_in(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        v = {"foo": "bar"}

        write_table("//tmp/t_in", v)

        merge(
            mode="ordered",
            in_=["//tmp/t_in", "//tmp/t_in", "//tmp/t_in"],
            out="//tmp/t_out",
        )

        assert read_table("//tmp/t_out") == [v, v, v]

    @authors("panin", "ignat")
    def test_in_equal_to_out(self):
        create("table", "//tmp/t_in")

        v = {"foo": "bar"}

        write_table("<append=true>//tmp/t_in", v)
        write_table("<append=true>//tmp/t_in", v)

        merge(
            combine_chunks=True,
            mode="ordered",
            in_="//tmp/t_in",
            out="<append=true>//tmp/t_in",
        )

        assert read_table("//tmp/t_in") == [v, v, v, v]
        assert get("//tmp/t_in/@chunk_count") == 3  # only result of merge is combined

    @authors("ignat")
    def test_selectors(self):
        self._prepare_tables()

        merge(
            mode="unordered",
            in_=[self.t1 + "[:#1]", self.t2 + "[#1:#2]"],
            out="//tmp/t_out",
        )

        assert_items_equal(read_table("//tmp/t_out"), self.v1[:1] + self.v2[1:2])
        assert get("//tmp/t_out/@chunk_count") == 2

    @authors("ignat")
    def test_column_selectors(self):
        self._prepare_tables()

        merge(mode="unordered", in_=[self.t1 + "{key1}"], out="//tmp/t_out")

        assert_items_equal(read_table("//tmp/t_out"), [self.v1[1], {}, {}])
        assert get("//tmp/t_out/@chunk_count") == 1

    @authors("savrus", "ermolovd")
    @pytest.mark.parametrize("mode", ["ordered", "unordered", "sorted"])
    def test_column_selectors_schema_inference(self, mode):
        create(
            "table",
            "//tmp/t",
            attributes={
                "schema": make_schema(
                    [
                        {"name": "k1", "type": "int64", "sort_order": "ascending"},
                        {"name": "k2", "type": "int64", "sort_order": "ascending"},
                        {"name": "v1", "type": "int64"},
                        {"name": "v2", "type": "int64"},
                    ],
                    unique_keys=True,
                )
            },
        )
        create("table", "//tmp/t_out")
        rows = [{"k1": i, "k2": i + 1, "v1": i + 2, "v2": i + 3} for i in xrange(2)]
        write_table("//tmp/t", rows)

        if mode != "sorted":
            merge(mode=mode, in_="//tmp/t{k1,v1}", out="//tmp/t_out")

            assert_items_equal(
                read_table("//tmp/t_out"),
                [{k: r[k] for k in ("k1", "v1")} for r in rows],
            )

            schema = make_schema(
                [
                    {"name": "k1", "type": "int64", "required": False},
                    {"name": "v1", "type": "int64", "required": False},
                ],
                unique_keys=False,
                strict=True,
            )
            if mode != "unordered":
                schema[0]["sort_order"] = "ascending"
            assert normalize_schema(get("//tmp/t_out/@schema")) == schema

            remove("//tmp/t_out")
            create("table", "//tmp/t_out")

            merge(mode=mode, in_="//tmp/t{k2,v2}", out="//tmp/t_out")

            assert_items_equal(
                read_table("//tmp/t_out"),
                [{k: r[k] for k in ("k2", "v2")} for r in rows],
            )

            schema = make_schema(
                [
                    {"name": "k2", "type": "int64", "required": False},
                    {"name": "v2", "type": "int64", "required": False},
                ],
                unique_keys=False,
                strict=True,
            )
            assert normalize_schema(get("//tmp/t_out/@schema")) == schema

            remove("//tmp/t_out")
            create("table", "//tmp/t_out")

        merge(mode=mode, in_="//tmp/t{k1,k2,v2}", out="//tmp/t_out")

        assert_items_equal(
            read_table("//tmp/t_out"),
            [{k: r[k] for k in ("k1", "k2", "v2")} for r in rows],
        )

        schema = make_schema(
            [
                {"name": "k1", "type": "int64", "required": False},
                {"name": "k2", "type": "int64", "required": False},
                {"name": "v2", "type": "int64", "required": False},
            ],
            unique_keys=False,
            strict=True,
        )
        if mode != "unordered":
            schema.attributes["unique_keys"] = True
            schema[0]["sort_order"] = "ascending"
            schema[1]["sort_order"] = "ascending"
        assert normalize_schema(get("//tmp/t_out/@schema")) == schema

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["ordered", "sorted"])
    def test_column_selectors_output_schema_validation(self, mode):
        create(
            "table",
            "//tmp/t",
            attributes={
                "schema": [
                    {"name": "key", "type": "int64", "sort_order": "ascending"},
                    {"name": "value", "type": "string"},
                ]
            },
        )
        create(
            "table",
            "//tmp/t_out",
            attributes={"schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]},
        )
        rows = [{"key": i, "value": str(i)} for i in xrange(2)]
        write_table("//tmp/t", rows)

        merge(mode=mode, in_="//tmp/t{key}", out="//tmp/t_out")

        assert_items_equal(read_table("//tmp/t_out"), [{"key": r["key"]} for r in rows])

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["ordered", "unordered"])
    def test_query_filtering(self, mode):
        create("table", "//tmp/t1", attributes={"schema": [{"name": "a", "type": "int64"}]})
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": i} for i in xrange(2)])

        merge(
            mode=mode,
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"input_query": "a where a > 0"},
        )

        assert read_table("//tmp/t2") == [{"a": 1}]
        assert get("//tmp/t2/@schema") == get("//tmp/t1/@schema")

        remove("//tmp/t2")
        create("table", "//tmp/t2")
        merge(
            mode=mode,
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"input_query": "a + 1 as b where a > 0"},
        )

        assert read_table("//tmp/t2") == [{"b": 2}]
        schema = get("//tmp/t1/@schema")
        schema[0]["name"] = "b"
        assert get("//tmp/t2/@schema") == schema

    @authors("savrus")
    def test_sorted_merge_query_filtering(self):
        create("table", "//tmp/t1", attributes={"schema": [{"name": "a", "type": "int64"}]})
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": i} for i in xrange(2)])

        with pytest.raises(YtError):
            merge(
                mode="sorted",
                in_="//tmp/t1",
                out="//tmp/t2",
                spec={"input_query": "a where a > 0"},
            )

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["ordered", "unordered"])
    def test_query_filtering_output_schema_validation(self, mode):
        create(
            "table",
            "//tmp/t",
            attributes={
                "schema": [
                    {"name": "key", "type": "int64", "sort_order": "ascending"},
                    {"name": "value", "type": "string"},
                ]
            },
        )
        sort_order = "ascending" if mode == "ordered" else None
        create(
            "table",
            "//tmp/t_out",
            attributes={"schema": [{"name": "k", "type": "int64", "sort_order": sort_order}]},
        )
        rows = [{"key": i, "value": str(i)} for i in xrange(2)]
        write_table("//tmp/t", rows)

        merge(
            mode=mode,
            in_="//tmp/t",
            out="//tmp/t_out",
            spec={"input_query": "key as k"},
        )

        assert_items_equal(read_table("//tmp/t_out"), [{"k": r["key"]} for r in rows])

    @authors("babenko")
    def test_merge_chunk_properties(self):
        create("table", "//tmp/t1", attributes={"replication_factor": 1, "vital": False})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id = get_singular_chunk_id("//tmp/t1")

        assert get_chunk_replication_factor(chunk_id) == 1
        assert not get("#" + chunk_id + "/@vital")

        create("table", "//tmp/t2", attributes={"replication_factor": 3, "vital": True})
        merge(mode="ordered", in_=["//tmp/t1"], out="//tmp/t2")

        wait(lambda: get_chunk_replication_factor(chunk_id) == 3 and get("#" + chunk_id + "/@vital"))

    @authors("ignat")
    def test_chunk_indices(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        for i in xrange(5):
            write_table("<sorted_by=[a];append=%true>//tmp/t1", [{"a": i}])

        merge(
            mode="sorted",
            in_=yson.to_yson_type(
                "//tmp/t1",
                attributes={
                    "ranges": [
                        {
                            "lower_limit": {"chunk_index": 1},
                            "upper_limit": {"chunk_index": 3},
                        }
                    ]
                },
            ),
            out="//tmp/t2",
        )

        assert read_table("//tmp/t2") == [{"a": i} for i in xrange(1, 3)]

    @authors("psushin")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_auto_schema_inference_unordered(self, optimize_for):
        loose_schema = make_schema([{"name": "key", "type": "int64"}], strict=False)
        strict_schema = make_schema([{"name": "key", "type": "int64"}])

        create("table", "//tmp/input_loose", attributes={"schema": loose_schema})
        create("table", "//tmp/input_weak")
        create("table", "//tmp/output_weak", attributes={"optimize_for": optimize_for})
        create(
            "table",
            "//tmp/output_loose",
            attributes={"optimize_for": optimize_for, "schema": loose_schema},
        )
        create(
            "table",
            "//tmp/output_strict",
            attributes={"optimize_for": optimize_for, "schema": strict_schema},
        )

        assert get("//tmp/input_loose/@schema_mode") == "strong"
        assert get("//tmp/output_weak/@schema_mode") == "weak"
        assert get("//tmp/output_loose/@schema_mode") == "strong"

        write_table("<append=true>//tmp/input_loose", {"key": 1, "value": "foo"})
        write_table("<append=true>//tmp/input_weak", {"key": 1, "value": "foo"})

        merge(in_="//tmp/input_loose", out="//tmp/output_weak")

        assert get("//tmp/output_weak/@schema_mode") == "strong"
        assert not get("//tmp/output_weak/@schema/@strict")

        merge(in_="//tmp/input_loose", out="//tmp/output_loose")
        assert get("//tmp/output_loose/@schema_mode") == "strong"
        assert not get("//tmp/output_loose/@schema/@strict")

        with pytest.raises(YtError):
            # changing from strict schema to nonstrict is not allowed
            merge(in_="//tmp/input_loose", out="//tmp/output_strict")

        # schema validation must pass
        merge(in_="//tmp/input_weak", out="//tmp/output_loose")
        assert get("//tmp/output_loose/@schema_mode") == "strong"
        assert not get("//tmp/output_loose/@schema/@strict")

        merge(in_=["//tmp/input_weak", "//tmp/input_loose"], out="//tmp/output_loose")

        create(
            "table",
            "//tmp/output_sorted",
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    strict=False,
                ),
            },
        )

        with pytest.raises(YtError):
            # cannot do unordered merge to sorted output
            merge(in_="//tmp/input_loose", out="//tmp/output_sorted")

        with pytest.raises(YtError):
            # even in user insists
            merge(
                in_="//tmp/input_loose",
                out="//tmp/output_sorted",
                spec={"schema_inference_mode": "from_output"},
            )

    @authors("psushin")
    def test_auto_schema_inference_ordered(self):
        output_schema = make_schema([{"name": "key", "type": "int64"}, {"name": "value", "type": "string"}])
        good_schema = make_schema([{"name": "key", "type": "int64"}])
        bad_schema = make_schema([{"name": "key", "type": "int64"}, {"name": "bad", "type": "int64"}])

        create("table", "//tmp/input_good", attributes={"schema": good_schema})
        create("table", "//tmp/input_bad", attributes={"schema": bad_schema})
        create("table", "//tmp/input_weak")
        create("table", "//tmp/output_strong", attributes={"schema": output_schema})
        create("table", "//tmp/output_weak")

        merge(in_=["//tmp/input_weak", "//tmp/input_good"], out="//tmp/output_strong")

        with pytest.raises(YtError):
            merge(in_=["//tmp/input_weak", "//tmp/input_bad"], out="//tmp/output_strong")

        with pytest.raises(YtError):
            merge(in_=["//tmp/input_weak", "//tmp/input_good"], out="//tmp/output_weak")

        with pytest.raises(YtError):
            merge(in_=["//tmp/input_bad", "//tmp/input_good"], out="//tmp/output_weak")

    @authors("babenko")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_schema_validation_unordered(self, optimize_for):
        create("table", "//tmp/input")
        create(
            "table",
            "//tmp/output",
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {"name": "key", "type": "int64"},
                        {"name": "value", "type": "string"},
                    ]
                ),
            },
        )

        for i in xrange(10):
            write_table("<append=true>//tmp/input", {"key": i, "value": "foo"})

        merge(
            in_="//tmp/input",
            out="//tmp/output",
            spec={"schema_inference_mode": "from_output"},
        )

        assert get("//tmp/output/@schema_mode") == "strong"
        assert get("//tmp/output/@schema/@strict")
        assert_items_equal(read_table("//tmp/output"), [{"key": i, "value": "foo"} for i in xrange(10)])

        write_table("//tmp/input", {"key": "1", "value": "foo"})

        with pytest.raises(YtError):
            merge(
                in_="//tmp/input",
                out="//tmp/output",
                spec={"schema_inference_mode": "from_output"},
            )

    @authors("babenko")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_schema_validation_ordered(self, optimize_for):
        create("table", "//tmp/input")
        create(
            "table",
            "//tmp/output",
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {"name": "key", "type": "int64"},
                        {"name": "value", "type": "string"},
                    ]
                ),
            },
        )

        for i in xrange(10):
            write_table("<append=true>//tmp/input", {"key": i, "value": "foo"})

        merge(
            mode="ordered",
            in_="//tmp/input",
            out="//tmp/output",
            spec={"schema_inference_mode": "from_output"},
        )

        assert get("//tmp/output/@schema_mode") == "strong"
        assert get("//tmp/output/@schema/@strict")
        assert read_table("//tmp/output") == [{"key": i, "value": "foo"} for i in xrange(10)]

        write_table("//tmp/input", {"key": "1", "value": "foo"})

        with pytest.raises(YtError):
            merge(
                mode="ordered",
                in_="//tmp/input",
                out="//tmp/output",
                spec={"schema_inference_mode": "from_output"},
            )

    @authors("babenko")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_schema_validation_sorted(self, optimize_for):
        create("table", "//tmp/input")
        create(
            "table",
            "//tmp/output",
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {"name": "key", "type": "int64", "sort_order": "ascending"},
                        {"name": "value", "type": "string"},
                    ]
                ),
            },
        )

        for i in xrange(10):
            write_table("<append=true; sorted_by=[key]>//tmp/input", {"key": i, "value": "foo"})

        assert get("//tmp/input/@sorted_by") == ["key"]

        merge(
            mode="sorted",
            in_="//tmp/input",
            out="//tmp/output",
            spec={"schema_inference_mode": "from_output"},
        )

        assert get("//tmp/output/@schema_mode") == "strong"
        assert get("//tmp/output/@schema/@strict")
        assert read_table("//tmp/output") == [{"key": i, "value": "foo"} for i in xrange(10)]

        write_table("<sorted_by=[key]>//tmp/input", {"key": "1", "value": "foo"})
        assert get("//tmp/input/@sorted_by") == ["key"]

        with pytest.raises(YtError):
            merge(
                mode="sorted",
                in_="//tmp/input",
                out="//tmp/output",
                spec={"schema_inference_mode": "from_output"},
            )

    @authors("ermolovd")
    @pytest.mark.parametrize("mode", ["unordered", "ordered", "sorted"])
    def test_schema_validation_complex_types(self, mode):
        first_column = {"name": "index", "type_v3": "int64"}
        if mode == "sorted":
            first_column["sort_order"] = "ascending"

        input_schema = make_schema(
            [
                first_column,
                {"name": "value", "type_v3": optional_type(optional_type("string"))},
            ],
            unique_keys=False,
            strict=True,
        )
        output_schema = make_schema(
            [
                first_column,
                {"name": "value", "type_v3": list_type(optional_type("string"))},
            ],
            unique_keys=False,
            strict=True,
        )

        create("table", "//tmp/input", attributes={"schema": input_schema})
        create("table", "//tmp/output", attributes={"schema": output_schema})
        write_table(
            "//tmp/input",
            [
                {"index": 1, "value": [None]},
                {"index": 2, "value": ["foo"]},
            ],
        )

        # We check that yson representation of types are compatible with each other
        write_table("//tmp/output", read_table("//tmp/input"))

        merge_by_args = {}
        if mode == "sorted":
            merge_by_args["merge_by"] = "index"

        with pytest.raises(YtError):
            merge(
                mode=mode,
                in_="//tmp/input",
                out="//tmp/output",
                spec={"schema_inference_mode": "auto"},
                **merge_by_args
            )
        merge(
            mode=mode,
            in_="//tmp/input",
            out="//tmp/output",
            spec={"schema_inference_mode": "from_output"},
            **merge_by_args
        )
        assert normalize_schema_v3(output_schema) == normalize_schema_v3(get("//tmp/output/@schema"))
        merge(
            mode=mode,
            in_="//tmp/input",
            out="//tmp/output",
            spec={"schema_inference_mode": "from_input"},
            **merge_by_args
        )
        assert normalize_schema_v3(input_schema) == normalize_schema_v3(get("//tmp/output/@schema"))

    @authors("savrus")
    @parametrize_external
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_sorted_merge_on_dynamic_table(self, external, optimize_for):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", optimize_for=optimize_for, external=external)

        create("table", "//tmp/t_out")

        rows1 = [{"key": i, "value": str(i)} for i in range(10)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows1)
        sync_unmount_table("//tmp/t")

        merge(mode="sorted", in_="//tmp/t", out="//tmp/t_out", merge_by="key")

        assert_items_equal(read_table("//tmp/t_out"), rows1)

        rows2 = [{"key": i, "value": str(i + 1)} for i in range(5, 15)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows2)
        sync_unmount_table("//tmp/t")

        merge(mode="sorted", in_="//tmp/t", out="//tmp/t_out", merge_by="key")

        assert_items_equal(read_table("//tmp/t_out"), rows1[:5] + rows2)

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["unordered", "ordered", "sorted"])
    def test_computed_columns(self, mode):
        create("table", "//tmp/t1")
        create(
            "table",
            "//tmp/t2",
            attributes={
                "schema": [
                    {"name": "k1", "type": "int64", "expression": "k2 * 2"},
                    {"name": "k2", "type": "int64"},
                ]
            },
        )

        write_table("<sorted_by=[k2]>//tmp/t1", [{"k2": i} for i in xrange(2)])

        merge(mode=mode, in_="//tmp/t1", out="//tmp/t2")

        assert get("//tmp/t2/@schema_mode") == "strong"
        assert read_table("//tmp/t2") == [{"k1": i * 2, "k2": i} for i in xrange(2)]

    @authors("psushin")
    def test_sort_order_validation_failure(self):
        create("table", "//tmp/input")
        create(
            "table",
            "//tmp/output",
            attributes={
                "schema": make_schema(
                    [
                        {"name": "key", "type": "int64", "sort_order": "ascending"},
                        {"name": "value", "type": "string"},
                    ]
                )
            },
        )

        for i in xrange(10):
            write_table("<append=true;>//tmp/input", {"key": i % 3, "value": "foo"})

        with pytest.raises(YtError):
            merge(
                mode="unordered",
                in_="//tmp/input",
                out="//tmp/output",
                spec={"schema_inference_mode": "from_output"},
            )

        with pytest.raises(YtError):
            merge(
                mode="ordered",
                in_="//tmp/input",
                out="//tmp/output",
                spec={"schema_inference_mode": "from_output"},
            )

    @authors("savrus")
    def test_writer_config(self):
        create("table", "//tmp/t_in")
        create(
            "table",
            "//tmp/t_out",
            attributes={
                "chunk_writer": {"block_size": 1024},
                "compression_codec": "none",
            },
        )

        write_table("//tmp/t_in", [{"value": "A" * 1024} for i in xrange(10)])

        merge(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"force_transform": "true", "job_count": 1},
        )

        chunk_id = get_singular_chunk_id("//tmp/t_out")
        assert get("#" + chunk_id + "/@compressed_data_size") > 1024 * 10
        assert get("#" + chunk_id + "/@max_block_size") < 1024 * 2

    @authors("max42")
    @pytest.mark.parametrize("mode", ["sorted", "ordered"])
    def test_merge_interrupt(self, mode):
        create(
            "table",
            "//tmp/t_in",
            attributes={"schema": [{"name": "a", "type": "int64", "sort_order": "ascending"}]},
        )
        create("table", "//tmp/t_out")
        for i in range(25):
            write_table("<append=%true>//tmp/t_in", {"a": i})
        op = merge(
            track=False,
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            spec={
                "force_transform": True,
                "mode": mode,
                "job_io": {
                    "testing_options": {"pipe_delay": 250},
                    "buffer_row_count": 1,
                },
                "enable_job_splitting": False,
            },
        )
        wait(lambda: len(op.get_running_jobs()) > 0)
        jobs = list(op.get_running_jobs())
        assert len(jobs) == 1
        job_id = jobs[0]
        wait(
            lambda: get(
                "//sys/scheduler/orchid/scheduler/jobs/{0}/progress".format(job_id),
                default=0,
            )
            >= 0.1
        )
        interrupt_job(job_id)
        op.track()
        rows = read_table("//tmp/t_out")
        assert get(op.get_path() + "/@progress/jobs/completed/total") == 2
        assert rows == [{"a": i} for i in range(25)]

    @authors("max42")
    @pytest.mark.parametrize("mode", ["sorted", "ordered"])
    def test_merge_job_splitter(self, mode):
        create(
            "table",
            "//tmp/t_in",
            attributes={
                "schema": [
                    {"name": "a", "type": "int64", "sort_order": "ascending"},
                    {"name": "b", "type": "string"},
                ]
            },
        )
        create("table", "//tmp/t_out")
        expected = []
        for i in range(20):
            row = {"a": i, "b": "x" * 10 ** 4}
            write_table("<append=%true>//tmp/t_in", row)
            expected.append(row)

        op = merge(
            track=False,
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            spec={
                "force_transform": True,
                "mode": mode,
                "job_io": {
                    "testing_options": {"pipe_delay": 500},
                    "buffer_row_count": 1,
                },
            },
        )

        wait(lambda: exists(op.get_path() + "/controller_orchid/progress/tasks/0/job_splitter"))

        op.track()

        completed = get(op.get_path() + "/@progress/jobs/completed")
        interrupted = completed["interrupted"]
        assert completed["total"] >= 2
        assert interrupted["job_split"] >= 1
        rows = read_table("//tmp/t_out", verbose=False)
        assert rows == expected

    @authors("max42")
    @pytest.mark.parametrize("mode", ["sorted", "ordered", "unordered"])
    def test_sampling(self, mode):
        create(
            "table",
            "//tmp/t1",
            attributes={
                "schema": [
                    {"name": "key", "type": "string", "sort_order": "ascending"},
                    {"name": "value", "type": "string"},
                ]
            },
        )
        create("table", "//tmp/t2")
        write_table(
            "//tmp/t1",
            [{"key": ("%02d" % (i // 100)), "value": "x" * 10 ** 2} for i in range(10000)],
            table_writer={"block_size": 1024},
        )

        op = merge(
            in_="//tmp/t1",
            out="//tmp/t2",
            mode=mode,
            spec={
                "sampling": {"sampling_rate": 0, "io_block_size": 10 ** 5},
                "data_weight_per_job": 10 ** 9,
                "enable_job_splitting": False,
            },
        )
        assert get("//tmp/t2/@row_count") == 0
        assert get("//tmp/t2/@row_count") == 0
        assert get(op.get_path() + "/@progress/jobs/total") == get(op.get_path() + "/@progress/jobs/completed/total")

        op = merge(
            in_="//tmp/t1",
            out="//tmp/t2",
            mode=mode,
            spec={
                "sampling": {"sampling_rate": 0.5, "io_block_size": 10 ** 5},
                "combine_chunks": True,
                "data_weight_per_job": 10 ** 9,
                "enable_job_splitting": False,
            },
        )
        assert 0.25 * 10000 <= get("//tmp/t2/@row_count") <= 0.75 * 10000
        assert get("//tmp/t2/@chunk_count") == 1
        assert get(op.get_path() + "/@progress/jobs/total") == get(op.get_path() + "/@progress/jobs/completed/total")

        op = merge(
            in_="//tmp/t1",
            out="//tmp/t2",
            mode=mode,
            spec={
                "sampling": {"sampling_rate": 0.5, "io_block_size": 10 ** 5},
                "job_count": 10,
                "combine_chunks": True,
                "enable_job_splitting": False,
            },
        )
        assert 0.25 * 10000 <= get("//tmp/t2/@row_count") <= 0.75 * 10000
        assert get("//tmp/t2/@chunk_count") > 1
        assert get(op.get_path() + "/@progress/jobs/total") == get(op.get_path() + "/@progress/jobs/completed/total")

        if mode != "unordered":
            op = merge(
                in_="//tmp/t1",
                out="//tmp/t2",
                mode=mode,
                spec={
                    "sampling": {"sampling_rate": 0.5, "io_block_size": 10 ** 7},
                    "job_count": 10,
                    "combine_chunks": True,
                    "data_weight_per_job": 10 ** 9,
                    "enable_job_splitting": False,
                },
            )
            assert get("//tmp/t2/@row_count") in [0, 10000]
            assert get("//tmp/t2/@chunk_count") in [0, 1]
            assert get(op.get_path() + "/@progress/jobs/total") == get(
                op.get_path() + "/@progress/jobs/completed/total"
            )

            op = merge(
                in_="//tmp/t1",
                out="//tmp/t2",
                mode=mode,
                spec={
                    "sampling": {
                        "sampling_rate": 0.5,
                        "io_block_size": 1,
                        "max_total_slice_count": 1,
                    },
                    "job_count": 10,
                    "combine_chunks": True,
                    "enable_job_splitting": False,
                },
            )
            assert get("//tmp/t2/@row_count") in [0, 10000]
            assert get("//tmp/t2/@chunk_count") in [0, 1]
            assert get(op.get_path() + "/@progress/jobs/total") == get(
                op.get_path() + "/@progress/jobs/completed/total"
            )

    @authors("max42")
    @pytest.mark.parametrize("mode", ["ordered", "unordered"])
    def test_zero_sampling_rate(self, mode):
        # YT-11640
        create(
            "table",
            "//tmp/t1",
            attributes={"schema": [{"name": "key", "type": "string"}]},
        )
        write_table(
            "//tmp/t1",
            [{"key": "0"} for i in range(1000000)],
            table_writer={"block_size": 1024},
        )
        for i in range(12):
            merge(in_=["//tmp/t1", "//tmp/t1"], out="//tmp/t1")
        create("table", "//tmp/t2")
        op = merge(
            track=False,
            in_="//tmp/t1",
            out="//tmp/t2",
            mode=mode,
            spec={
                "sampling": {"sampling_rate": 0, "io_block_size": 10 ** 5},
                "data_weight_per_job": 10 ** 9,
                "enable_job_splitting": False,
                "combine_chunks": True,
            },
        )
        wait(lambda: op.get_state().endswith("ed"))
        op.track()

        assert get("//tmp/t2/@row_count") == 0
        assert get("//tmp/t2/@row_count") == 0
        assert get(op.get_path() + "/@progress/jobs/total") == get(op.get_path() + "/@progress/jobs/completed/total")

    @authors("max42")
    @pytest.mark.parametrize("mode", ["sorted", "ordered", "unordered"])
    def test_sampling_teleport(self, mode):
        create(
            "table",
            "//tmp/t1",
            attributes={"schema": [{"name": "key", "type": "string", "sort_order": "ascending"}]},
        )
        create("table", "//tmp/t2")
        for i in range(100):
            write_table("<append=%true>//tmp/t1", [{"key": ("%02d" % i)}])

        merge(
            in_="//tmp/t1",
            out="//tmp/t2",
            mode=mode,
            spec={
                "sampling": {
                    "sampling_rate": 0.1,
                    "user_limits": {"resource_limits": {"user_slots": 0}},
                },
                "enable_job_splitting": False,
            },
        )
        assert 0 <= get("//tmp/t2/@chunk_count") <= 20

    @authors("max42", "psushin")
    def test_overlapping_ranges_in_sorted_merge(self):
        create(
            "table",
            "//tmp/t1",
            attributes={"schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]},
        )
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"key": 0}, {"key": 1}])

        merge(in_="<ranges=[{};{}]>//tmp/t1", out="//tmp/t2", mode="sorted")

        assert read_table("//tmp/t2") == [
            {"key": 0},
            {"key": 0},
            {"key": 1},
            {"key": 1},
        ]

    @authors("ermolovd")
    def test_schema_compatibility(self):
        create(
            "table",
            "//tmp/t1",
            attributes={"schema": [{"name": "key", "type": "int64"}]},
        )
        write_table("//tmp/t1", [{"key": None}])
        with pytest.raises(YtError):
            merge(
                in_="//tmp/t1",
                out="<schema=[{name=key;type=int64;required=true}]>//tmp/t2",
            )

    @authors("ermolovd")
    @pytest.mark.parametrize("mode", ["unordered", "ordered"])
    def test_infer_output_yt_8661_first(self, mode):
        schema = make_schema([{"name": "x", "type": "uint64"}], strict=False)
        schemaful_table = "//tmp/schemaful_table"
        schemaless_table = "//tmp/schemaless_table"

        create(
            "table",
            schemaful_table,
            attributes={
                "schema": schema,
                "optimize_for": "scan",
            },
        )
        create("table", schemaless_table)

        write_table(schemaful_table, [{"x": i} for i in xrange(100)])
        write_table(schemaless_table, [{"x": str(i)} for i in xrange(100, 200)])

        # merging non-strict table with strict table
        with pytest.raises(YtError):
            merge(
                mode=mode,
                in_=[schemaless_table, schemaful_table],
                out=schemaful_table,
                spec={"schema_inference_mode": "from_output"},
            )

    @authors("ermolovd")
    @pytest.mark.parametrize("mode", ["unordered", "ordered"])
    def test_infer_output_yt_8661_second(self, mode):
        schema1 = make_schema(
            [
                {"name": "x", "type": "uint64", "required": True},
                {"name": "y", "type": "uint64", "required": True},
            ]
        )
        schema2 = make_schema(
            [
                {"name": "y", "type": "uint64", "required": True},
            ]
        )
        table1 = "//tmp/table1"
        table2 = "//tmp/table2"

        create(
            "table",
            table1,
            attributes={
                "schema": schema1,
            },
        )
        create(
            "table",
            table2,
            attributes={
                "schema": schema2,
            },
        )

        write_table(table1, [{"x": i, "y": i} for i in xrange(100)])
        write_table(table2, [{"y": i} for i in xrange(100, 200)])

        with pytest.raises(YtError):
            merge(
                mode=mode,
                in_=[table2, table1],
                out=table1,
                spec={"schema_inference_mode": "from_output"},
            )

    @authors("ermolovd")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_merge_lose_composite_type(self, optimize_for):
        create(
            "table",
            "//tmp/t1",
            attributes={
                "schema": [
                    {"name": "a", "type_v3": list_type("int64")},
                ]
            },
        )
        write_table("//tmp/t1", [{"a": [1, 2, 3]}, {"a": [4, 5, 6]}])

        create(
            "table",
            "//tmp/t_out",
            attributes={
                "schema": [
                    {"name": "a", "type_v3": optional_type("yson")},
                ],
                "optimize_for": optimize_for,
            },
        )
        op = merge(
            in_=["//tmp/t1"],
            out="//tmp/t_out",
            spec={"schema_inference_mode": "from_output"},
        )


##################################################################


class TestSchedulerMergeCommandsSliceSize(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "operations_update_period": 10,
            "running_jobs_update_period": 10,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10,
            "sorted_merge_operation_options": {
                "min_slice_data_weight": 1,
            },
        }
    }

    DELTA_NODE_CONFIG = {"scheduler_connector": {"heartbeat_period": 100}}  # 100 msec

    @authors("gritukan")
    @pytest.mark.skipif("True", reason="YT-13230")
    @pytest.mark.parametrize("tables_intersect", [False, True])
    def test_chunk_slice_size(self, tables_intersect):
        for i in range(10):
            create(
                "table",
                "//tmp/in{}".format(i),
                attributes={
                    "schema": [{"name": "key", "type": "string", "sort_order": "ascending"}],
                    "chunk_writer": {"block_size": 1},  # Each block should have exactly one row to make precise slices.
                    "compression_codec": "none",
                },
            )
            if tables_intersect:
                write_table(
                    "//tmp/in{}".format(i),
                    [{"key": ("%04d" % (10 * x + i))} for x in range(10)],
                )
            else:
                # Last row ensures that chunk won't be teleported.
                write_table(
                    "//tmp/in{}".format(i),
                    [{"key": ("%04d" % (10 * i + x))} for x in range(9)] + [{"key": ("%04d" % (9000 + i))}],
                )
        create("table", "//tmp/out")

        op = merge(
            mode="sorted",
            in_=["//tmp/in{}".format(i) for i in range(10)],
            out="//tmp/out",
            spec={"job_count": 10, "enable_job_splitting": False},
        )
        op.track()
        for chunk_id in get("//tmp/out/@chunk_ids"):
            assert 5 <= get("#" + chunk_id + "/@row_count") <= 15


##################################################################


class TestSchedulerMergeCommandsMulticell(TestSchedulerMergeCommands):
    NUM_SECONDARY_MASTER_CELLS = 2

    @authors("babenko")
    def test_multicell_merge_teleport(self):
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id1 = get_singular_chunk_id("//tmp/t1")

        create("table", "//tmp/t2", attributes={"external_cell_tag": 2})
        write_table("//tmp/t2", [{"a": 2}])
        chunk_id2 = get_singular_chunk_id("//tmp/t2")

        assert get("#" + chunk_id1 + "/@ref_counter") == 1
        assert get("#" + chunk_id2 + "/@ref_counter") == 1
        assert_items_equal(get("#" + chunk_id1 + "/@exports"), {})
        assert_items_equal(get("#" + chunk_id2 + "/@exports"), {})

        create("table", "//tmp/t", attributes={"external": False})
        merge(mode="ordered", in_=["//tmp/t1", "//tmp/t2"], out="//tmp/t")

        assert get("//tmp/t/@chunk_ids") == [chunk_id1, chunk_id2]
        assert get("#" + chunk_id1 + "/@ref_counter") == 2
        assert get("#" + chunk_id2 + "/@ref_counter") == 2
        wait(
            lambda: get("#" + chunk_id1 + "/@exports")
            == {
                "0": {
                    "ref_counter": 1,
                    "vital": True,
                    "media": {"default": {"replication_factor": 3, "data_parts_only": False}},
                }
            }
        )
        wait(
            lambda: get("#" + chunk_id2 + "/@exports")
            == {
                "0": {
                    "ref_counter": 1,
                    "vital": True,
                    "media": {"default": {"replication_factor": 3, "data_parts_only": False}},
                }
            }
        )
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(0)), [chunk_id1, chunk_id2])

        assert read_table("//tmp/t") == [{"a": 1}, {"a": 2}]

        remove("//tmp/t")

        wait(
            lambda: get("#" + chunk_id1 + "/@ref_counter") == 1
            and get("#" + chunk_id2 + "/@ref_counter") == 1
            and get("#" + chunk_id1 + "/@exports") == {}
            and get("#" + chunk_id2 + "/@exports") == {}
            and ls("//sys/foreign_chunks", driver=get_driver(0)) == []
        )

    @authors("babenko")
    def test_multicell_merge_multi_teleport(self):
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id = get_singular_chunk_id("//tmp/t1")

        assert get("#" + chunk_id + "/@ref_counter") == 1
        assert get("#" + chunk_id + "/@exports") == {}
        assert not get("#" + chunk_id + "/@foreign")
        assert not exists("#" + chunk_id, driver=get_driver(2))

        create("table", "//tmp/t2", attributes={"external_cell_tag": 2})
        merge(mode="ordered", in_=["//tmp/t1", "//tmp/t1"], out="//tmp/t2")

        assert get("//tmp/t2/@chunk_ids") == [chunk_id, chunk_id]
        assert get("#" + chunk_id + "/@ref_counter") == 3
        wait(
            lambda: get("#" + chunk_id + "/@exports")
            == {
                "2": {
                    "ref_counter": 2,
                    "vital": True,
                    "media": {"default": {"replication_factor": 3, "data_parts_only": False}},
                }
            }
        )
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(2)), [chunk_id])
        assert get("#" + chunk_id + "/@import_ref_counter", driver=get_driver(2)) == 2

        assert read_table("//tmp/t2") == [{"a": 1}, {"a": 1}]

        create("table", "//tmp/t3", attributes={"external_cell_tag": 2})
        merge(mode="ordered", in_=["//tmp/t1", "//tmp/t1"], out="//tmp/t3")

        assert get("//tmp/t3/@chunk_ids") == [chunk_id, chunk_id]
        assert get("#" + chunk_id + "/@ref_counter") == 5
        wait(
            lambda: get("#" + chunk_id + "/@exports")
            == {
                "2": {
                    "ref_counter": 4,
                    "vital": True,
                    "media": {"default": {"replication_factor": 3, "data_parts_only": False}},
                }
            }
        )
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(2)), [chunk_id])
        assert get("#" + chunk_id + "/@import_ref_counter", driver=get_driver(2)) == 4

        assert read_table("//tmp/t3") == [{"a": 1}, {"a": 1}]

        remove("//tmp/t2")

        wait(
            lambda: get("#" + chunk_id + "/@ref_counter") == 5
            and get("#" + chunk_id + "/@exports")
            == {
                "2": {
                    "ref_counter": 4,
                    "vital": True,
                    "media": {"default": {"replication_factor": 3, "data_parts_only": False}},
                }
            }
            and ls("//sys/foreign_chunks", driver=get_driver(2)) == [chunk_id]
        )

        remove("//tmp/t3")

        wait(
            lambda: get("#" + chunk_id + "/@ref_counter") == 1
            and get("#" + chunk_id + "/@exports") == {}
            and ls("//sys/foreign_chunks", driver=get_driver(2)) == []
        )

        remove("//tmp/t1")

        wait(lambda: not exists("#" + chunk_id))

    @authors("babenko")
    def test_multicell_merge_chunk_properties(self):
        create(
            "table",
            "//tmp/t1",
            attributes={
                "replication_factor": 1,
                "vital": False,
                "external_cell_tag": 1,
            },
        )
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id = get_singular_chunk_id("//tmp/t1")

        assert get_chunk_replication_factor(chunk_id) == 1
        assert not get("#" + chunk_id + "/@vital")

        create(
            "table",
            "//tmp/t2",
            attributes={
                "replication_factor": 3,
                "vital": False,
                "external_cell_tag": 2,
            },
        )
        merge(mode="ordered", in_=["//tmp/t1"], out="//tmp/t2")

        wait(lambda: get_chunk_replication_factor(chunk_id) == 3 and not get("#" + chunk_id + "/@vital"))

        set("//tmp/t2/@replication_factor", 2)

        wait(lambda: get_chunk_replication_factor(chunk_id) == 2)

        set("//tmp/t2/@replication_factor", 3)

        wait(lambda: get_chunk_replication_factor(chunk_id) == 3)

        set("//tmp/t2/@vital", True)

        wait(lambda: get("#" + chunk_id + "/@vital"))

        set("//tmp/t1/@replication_factor", 4)

        wait(lambda: get_chunk_replication_factor(chunk_id) == 4)

        set("//tmp/t1/@replication_factor", 1)

        wait(lambda: get_chunk_replication_factor(chunk_id) == 3)

        remove("//tmp/t2")

        wait(lambda: get_chunk_replication_factor(chunk_id) == 1 and not get("#" + chunk_id + "/@vital"))

    @authors("babenko")
    def test_yt_4259(self):
        create("table", "//tmp/t", attributes={"external": False})
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        create("table", "//tmp/t2", attributes={"external_cell_tag": 2})

        write_table("//tmp/t", [{"a": 1}])
        chunk_id = get_singular_chunk_id("//tmp/t")

        merge(mode="ordered", in_=["//tmp/t"], out="//tmp/t1")
        merge(mode="ordered", in_=["//tmp/t"], out="//tmp/t2")

        wait(
            lambda: get("#" + chunk_id + "/@exports")
            == {
                "1": {
                    "ref_counter": 1,
                    "vital": True,
                    "media": {"default": {"replication_factor": 3, "data_parts_only": False}},
                },
                "2": {
                    "ref_counter": 1,
                    "vital": True,
                    "media": {"default": {"replication_factor": 3, "data_parts_only": False}},
                },
            }
        )

    @authors("shakurov")
    def test_teleporting_chunks_dont_disappear(self):
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id = get_singular_chunk_id("//tmp/t1")

        create("table", "//tmp/t2", attributes={"external_cell_tag": 2})

        tx = start_transaction()
        merge(mode="ordered", in_=["//tmp/t1"], out="//tmp/t2", tx=tx)

        assert get("//tmp/t2/@chunk_count", tx=tx) == 1
        assert get_singular_chunk_id("//tmp/t2", tx=tx) == chunk_id

        assert get("#{0}/@exports/2/ref_counter".format(chunk_id)) == 1

        # The point of this test is to make sure snatching chunks from
        # under an uncommitted transaction interoperates with
        # multicell well. Replacing the following two lines with this:
        #     copy("//tmp/t2", "//tmp/t2_copy", source_transaction_id=tx)
        # used to produce (it's no longer supported) a horrific situation
        # when a chunk is destroyed in its cell yet is still
        # referenced from another cell.
        create("table", "//tmp/t2_copy", attributes={"external_cell_tag": 2})
        merge(
            mode="ordered",
            in_=['<transaction_id="{0}">//tmp/t2'.format(tx)],
            out="//tmp/t2_copy",
        )

        abort_transaction(tx)

        remove("//tmp/t1")

        # Give replicator a chance to remove a chunk (in case there's a bug).
        # NB: This sleep cannot be replaced with wait.
        sleep(1.0)

        assert exists("//tmp/t2")
        assert get("//tmp/t2/@chunk_count") == 0
        assert exists("//tmp/t2_copy")
        assert get("//tmp/t2_copy/@chunk_count") == 1
        assert get_singular_chunk_id("//tmp/t2_copy") == chunk_id
        assert exists("#{0}".format(chunk_id))

    @authors("babenko")
    def test_fetch_trimmed_ordered_table_yt_11825(self):
        sync_create_cells(1)
        create(
            "table",
            "//tmp/in",
            attributes={
                "dynamic": True,
                "schema": [{"name": "value", "type": "int64"}],
            },
        )
        sync_mount_table("//tmp/in")

        n = 20
        for i in xrange(n):
            insert_rows("//tmp/in", [{"value": i}])
            sync_freeze_table("//tmp/in")
            sync_unfreeze_table("//tmp/in")

        assert get("//tmp/in/@chunk_count") == n

        m = 15
        trim_rows("//tmp/in", 0, m)

        wait(lambda: get("//tmp/in/@chunk_count") == n - m)

        create("table", "//tmp/out")

        merge(mode="ordered", in_=["//tmp/in"], out="//tmp/out")

        assert read_table("//tmp/out") == [{"value": i} for i in xrange(m, n)]
