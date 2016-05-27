import pytest

from yt_env_setup import YTEnvSetup, make_schema, unix_only
from yt_commands import *

from yt.environment.helpers import assert_items_equal
from time import sleep

##################################################################

class TestSchedulerMergeCommands(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    def _prepare_tables(self):
        t1 = "//tmp/t1"
        create("table", t1)
        v1 = [{"key" + str(i) : "value" + str(i)} for i in xrange(3)]
        for v in v1:
            write_table("<append=true>" + t1, v)

        t2 = "//tmp/t2"
        create("table", t2)
        v2 = [{"another_key" + str(i) : "another_value" + str(i)} for i in xrange(4)]
        for v in v2:
            write_table("<append=true>" + t2, v)

        self.t1 = t1
        self.t2 = t2

        self.v1 = v1
        self.v2 = v2

        create("table", "//tmp/t_out")

    # usual cases
    def test_unordered(self):
        self._prepare_tables()

        merge(mode="unordered",
              in_=[self.t1, self.t2],
              out="//tmp/t_out")

        assert_items_equal(read_table("//tmp/t_out"), self.v1 + self.v2)
        assert get("//tmp/t_out/@chunk_count") == 7

    def test_unordered_combine(self):
        self._prepare_tables()

        merge(combine_chunks=True,
              mode="unordered",
              in_=[self.t1, self.t2],
              out="//tmp/t_out")

        assert_items_equal(read_table("//tmp/t_out"), self.v1 + self.v2)
        assert get("//tmp/t_out/@chunk_count") == 1

    def test_unordered_with_mixed_chunks(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"a": 4}, {"a": 5}, {"a": 6}])
        write_table("//tmp/t2", [{"a": 7}, {"a": 8}, {"a": 9}])
        write_table("//tmp/t3", [{"a": 1}, {"a": 2}, {"a": 3}])

        create("table", "//tmp/t_out")
        merge(mode="unordered",
              in_=["//tmp/t1", "//tmp/t2[:#2]", "//tmp/t3[#1:]"],
              out="//tmp/t_out",
              spec={"data_size_per_job": 1000})

        assert get("//tmp/t_out/@chunk_count") == 2
        assert get("//tmp/t_out/@row_count") == 7
        assert sorted(read_table("//tmp/t_out")) == [{"a": i} for i in range(2, 9)]

    def test_ordered(self):
        self._prepare_tables()

        merge(mode="ordered",
              in_=[self.t1, self.t2],
              out="//tmp/t_out")

        assert read_table("//tmp/t_out") == self.v1 + self.v2
        assert get("//tmp/t_out/@chunk_count") ==7

    def test_ordered_combine(self):
        self._prepare_tables()

        merge(combine_chunks=True,
              mode="ordered",
              in_=[self.t1, self.t2],
              out="//tmp/t_out")

        assert read_table("//tmp/t_out") == self.v1 + self.v2
        assert get("//tmp/t_out/@chunk_count") == 1

    def test_sorted(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        write_table("//tmp/t1", [{"a": 1}, {"a": 10}, {"a": 100}], sorted_by="a")
        write_table("//tmp/t2", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(mode="sorted",
              in_=["//tmp/t1", "//tmp/t2"],
              out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [{"a": 1}, {"a": 2}, {"a": 3}, {"a": 10}, {"a": 15}, {"a": 100}]
        assert get("//tmp/t_out/@chunk_count") == 1 # resulting number of chunks is always equal to 1 (as long as they are small)
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["a"]

    def test_sorted_trivial(self):
        create("table", "//tmp/t1")

        write_table("//tmp/t1", [{"a": 1}, {"a": 10}, {"a": 100}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(combine_chunks=True,
              mode="sorted",
              in_=["//tmp/t1"],
              out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [{"a": 1}, {"a": 10}, {"a": 100}]
        assert get("//tmp/t_out/@chunk_count") == 1 # resulting number of chunks is always equal to 1 (as long as they are small)
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["a"]

    def test_append_not_sorted(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_out", [{"a": 1}, {"a": 2}, {"a": 3}], sorted_by="a")
        write_table("//tmp/t_in", [{"a": 0}])

        merge(mode="unordered",
              in_=["//tmp/t_in"],
              out="<append=true>//tmp/t_out")

        assert get("//tmp/t_out/@sorted") == False

    def test_sorted_with_same_chunks(self):
        t1 = "//tmp/t1"
        t2 = "//tmp/t2"
        v = [{"key1" : "value1"}]

        create("table", t1)
        write_table(t1, v[0])
        sort(in_=t1,
             out=t1,
             sort_by="key1")
        copy(t1, t2)

        create("table", "//tmp/t_out")
        merge(mode="sorted",
              in_=[t1, t2],
              out="//tmp/t_out")
        assert_items_equal(read_table("//tmp/t_out"), v + v)

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["key1"]

    def test_sorted_combine(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        write_table("//tmp/t1", [{"a": 1}, {"a": 10}, {"a": 100}], sorted_by="a")
        write_table("//tmp/t2", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(combine_chunks=True,
              mode="sorted",
              in_=["//tmp/t1", "//tmp/t2"],
              out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [{"a": 1}, {"a": 2}, {"a": 3}, {"a": 10}, {"a": 15}, {"a": 100}]
        assert get("//tmp/t_out/@chunk_count") == 1
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["a"]

    def test_sorted_passthrough(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"k": "a", "s": 0}, {"k": "b", "s": 1}], sorted_by=["k", "s"])
        write_table("//tmp/t2", [{"k": "b", "s": 2}, {"k": "c", "s": 0}], sorted_by=["k", "s"])
        write_table("//tmp/t3", [{"k": "b", "s": 0}, {"k": "b", "s": 3}], sorted_by=["k", "s"])

        create("table", "//tmp/t_out")
        merge(mode="sorted",
              in_=["//tmp/t1", "//tmp/t2", "//tmp/t3", "//tmp/t2[(b, 3) : (b, 7)]"],
              out="//tmp/t_out",
              merge_by="k")

        res = read_table("//tmp/t_out")
        expected = [
            {"k" : "a", "s" : 0},
            {"k" : "b", "s" : 1},
            {"k" : "b", "s" : 0},
            {"k" : "b", "s" : 3},
            {"k" : "b", "s" : 2},
            {"k" : "c", "s" : 0}]

        assert_items_equal(res, expected)

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["k"]

        merge(mode="sorted",
              in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
              out="//tmp/t_out",
              merge_by="k")

        res = read_table("//tmp/t_out")
        assert_items_equal(res, expected)

        assert get("//tmp/t_out/@chunk_count") == 3
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["k"]

        merge(mode="sorted",
              in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
              out="//tmp/t_out",
              merge_by=["k", "s"])

        res = read_table("//tmp/t_out")
        expected = [
            {"k" : "a", "s" : 0},
            {"k" : "b", "s" : 0},
            {"k" : "b", "s" : 1},
            {"k" : "b", "s" : 2},
            {"k" : "b", "s" : 3},
            {"k" : "c", "s" : 0} ]

        for i, j in zip(res, expected):
            assert i == j

        assert get("//tmp/t_out/@chunk_count") == 1
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["k", "s"]

    def test_sorted_with_maniacs(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        write_table("//tmp/t1", [{"a": 3}, {"a": 3}, {"a": 3}], sorted_by="a")
        write_table("//tmp/t2", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")
        write_table("//tmp/t3", [{"a": 1}, {"a": 3}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(combine_chunks=True,
              mode="sorted",
              in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
              out="//tmp/t_out",
              spec={"data_size_per_job": 1})

        assert read_table("//tmp/t_out") == [{"a": 1}, {"a": 2}, {"a": 3}, {"a": 3}, {"a": 3}, {"a": 3}, {"a": 3}, {"a": 15}]
        assert get("//tmp/t_out/@chunk_count") == 3
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["a"]

    def test_sorted_with_row_limits(self):
        create("table", "//tmp/t1")

        write_table("//tmp/t1", [{"a": 2}, {"a": 3}, {"a": 15}], sorted_by="a")

        create("table", "//tmp/t_out")
        merge(combine_chunks=False,
              mode="sorted",
              in_="//tmp/t1[:#2]",
              out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [{"a": 2}, {"a": 3}]
        assert get("//tmp/t_out/@chunk_count") == 1

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
            merge(mode="sorted",
                  in_=["//tmp/t1", "//tmp/t2"],
                  out="//tmp/t_out")

        # now merge_by is set
        merge(mode="sorted",
              in_=["//tmp/t1", "//tmp/t2"],
              out="//tmp/t_out",
              merge_by="a")

        result = read_table("//tmp/t_out")
        assert result[:2] == [a1, b1]
        assert_items_equal(result[2:5], [a2, a3, b2])
        assert result[5] == b3

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["a"]

    def test_sorted_unique_simple(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")
        create("table", "//tmp/t_out", attributes={
            "schema": make_schema([
                {"name": "a", "type": "int64", "sort_order": "ascending"},
                {"name": "b", "type": "int64"}],
                unique_keys=True)
            })

        a1 = {"a": 1, "b": 1}
        a2 = {"a": 2, "b": 2}
        a3 = {"a": 3, "b": 3}

        write_table("//tmp/t1", [a1, a2], sorted_by=["a", "b"])
        write_table("//tmp/t2", [a3], sorted_by=["a"])
        write_table("//tmp/t3", [a3, a3], sorted_by=["a", "b"])

        with pytest.raises(YtError):
            merge(mode="sorted",
                  in_="//tmp/t3",
                  out="//tmp/t_out",
                  merge_by="a")

        merge(mode="sorted",
              in_=["//tmp/t1", "//tmp/t2"],
              out="//tmp/t_out",
              merge_by="a")

        result = read_table("//tmp/t_out")
        assert result == [a1, a2, a3]

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") ==  ["a"]
        assert get("//tmp/t_out/@schema/@unique_keys")

    def test_sorted_unique_with_wider_key_columns(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t_out", attributes={
            "schema": make_schema([
                {"name": "key1", "type": "int64", "sort_order": "ascending"},
                {"name": "key2", "type": "int64", "sort_order": "ascending"}],
                unique_keys=True)
            })

        write_table(
            "//tmp/t1",
            [{"key1": 1, "key2": 1}, {"key1": 1, "key2": 2}],
            sorted_by=["key1", "key2"])

        with pytest.raises(YtError):
            merge(mode="sorted",
                in_="//tmp/t1",
                out="//tmp/t_out",
                merge_by="key1")

        merge(mode="sorted",
            in_="//tmp/t1",
            out="//tmp/t_out",
            merge_by=["key1", "key2"])

        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["key1", "key2"]
        assert get("//tmp/t_out/@schema/@unique_keys") == True

    def test_empty_in(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t_out")

        v = {"foo": "bar"}
        write_table("//tmp/t1", v)

        merge(mode="ordered",
               in_=["//tmp/t1", "//tmp/t2"],
               out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [v]

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

        merge(mode="ordered",
               in_=["//tmp/t1", "//tmp/t2"],
               out="<append=true>//tmp/t_out")

        assert read_table("//tmp/t_out") == [v3, v1, v2]

    def test_multiple_in(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        v = {"foo": "bar"}

        write_table("//tmp/t_in", v)

        merge(mode="ordered",
               in_=["//tmp/t_in", "//tmp/t_in", "//tmp/t_in"],
               out="//tmp/t_out")

        assert read_table("//tmp/t_out") == [v, v, v]

    def test_in_equal_to_out(self):
        create("table", "//tmp/t_in")

        v = {"foo": "bar"}

        write_table("<append=true>//tmp/t_in", v)
        write_table("<append=true>//tmp/t_in", v)


        merge(combine_chunks=True,
               mode="ordered",
               in_="//tmp/t_in",
               out="<append=true>//tmp/t_in")

        assert read_table("//tmp/t_in") == [v, v, v, v]
        assert get("//tmp/t_in/@chunk_count") == 3 # only result of merge is combined

    def test_selectors(self):
        self._prepare_tables()

        merge(mode="unordered",
              in_=[self.t1 + "[:#1]", self.t2 + "[#1:#2]"],
              out="//tmp/t_out")

        assert_items_equal(read_table("//tmp/t_out"), self.v1[:1] + self.v2[1:2])
        assert get("//tmp/t_out/@chunk_count") == 2

    def test_column_selectors(self):
        self._prepare_tables()

        merge(mode="unordered",
              in_=[self.t1 + "{key1}"],
              out="//tmp/t_out")

        assert_items_equal(read_table("//tmp/t_out"), [self.v1[1], {}, {}])
        assert get("//tmp/t_out/@chunk_count") == 1

    @unix_only
    def test_query_filtering(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": i} for i in xrange(2)])

        merge(mode="unordered",
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={
                "force_transform": "true",
                "input_query": "a where a > 0",
                "input_schema": [{"name": "a", "type": "int64"}]})

        assert read_table("//tmp/t2") == [{"a": 1}]

    @unix_only
    def test_merge_chunk_properties(self):
        create("table", "//tmp/t1", attributes={"replication_factor": 1, "vital": False})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id = get("//tmp/t1/@chunk_ids/0")

        assert get("#" + chunk_id + "/@replication_factor") == 1
        assert not get("#" + chunk_id + "/@vital")

        create("table", "//tmp/t2", attributes={"replication_factor": 3, "vital": True})
        merge(mode="ordered",
              in_=["//tmp/t1"],
              out="//tmp/t2")

        sleep(0.2)
        assert get("#" + chunk_id + "/@replication_factor") == 3
        assert get("#" + chunk_id + "/@vital")

    @unix_only
    def test_chunk_indices(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        for i in xrange(5):
            write_table("<sorted_by=[a];append=%true>//tmp/t1", [{"a": i}])

        merge(mode="sorted",
            in_=yson.to_yson_type("//tmp/t1", attributes={"ranges": [
                {
                    "lower_limit": {"chunk_index": 1},
                    "upper_limit": {"chunk_index": 3}
                }
            ]}),
            out="//tmp/t2")

        assert read_table("//tmp/t2") == [{"a": i} for i in xrange(1, 3)]

    def test_schema_validation_unordered(self):
        create("table", "//tmp/input")
        create("table", "//tmp/output", attributes={
            "schema": make_schema([
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"}])
            })

        for i in xrange(10):
            write_table("<append=true>//tmp/input", {"key": i, "value": "foo"})

        merge(in_="//tmp/input",
            out="//tmp/output")

        assert get("//tmp/output/@preserve_schema_on_write")
        assert get("//tmp/output/@schema/@strict")
        assert_items_equal(read_table("//tmp/output"), [{"key": i, "value": "foo"} for i in xrange(10)])

        write_table("//tmp/input", {"key": "1", "value": "foo"})

        with pytest.raises(YtError):
            merge(in_="//tmp/input",
                out="//tmp/output")

    def test_schema_validation_ordered(self):
        create("table", "//tmp/input")
        create("table", "//tmp/output", attributes={
            "schema": make_schema([
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"}])
            })

        for i in xrange(10):
            write_table("<append=true>//tmp/input", {"key": i, "value": "foo"})

        merge(mode="ordered",
            in_="//tmp/input",
            out="//tmp/output")

        assert get("//tmp/output/@preserve_schema_on_write")
        assert get("//tmp/output/@schema/@strict")
        assert read_table("//tmp/output") == [{"key": i, "value": "foo"} for i in xrange(10)]

        write_table("//tmp/input", {"key": "1", "value": "foo"})

        with pytest.raises(YtError):
            merge(mode="ordered",
                in_="//tmp/input",
                out="//tmp/output")

    def test_schema_validation_sorted(self):
        create("table", "//tmp/input")
        create("table", "//tmp/output", attributes={
            "schema": make_schema([
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "string"}])
            })

        for i in xrange(10):
            write_table("<append=true; sorted_by=[key]>//tmp/input", {"key": i, "value": "foo"})

        assert get("//tmp/input/@sorted_by") == ["key"]

        merge(mode="sorted",
            in_="//tmp/input",
            out="//tmp/output")

        assert get("//tmp/output/@preserve_schema_on_write")
        assert get("//tmp/output/@schema/@strict")
        assert read_table("//tmp/output") == [{"key": i, "value": "foo"} for i in xrange(10)]

        write_table("<sorted_by=[key]>//tmp/input", {"key": "1", "value": "foo"})
        assert get("//tmp/input/@sorted_by") == ["key"]

        with pytest.raises(YtError):
            merge(mode="sorted",
                in_="//tmp/input",
                out="//tmp/output")

##################################################################

class TestSchedulerMergeCommandsMulticell(TestSchedulerMergeCommands):
    NUM_SECONDARY_MASTER_CELLS = 2

    @unix_only
    def test_multicell_merge_teleport(self):
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id1 = get("//tmp/t1/@chunk_ids/0")

        create("table", "//tmp/t2", attributes={"external_cell_tag": 2})
        write_table("//tmp/t2", [{"a": 2}])
        chunk_id2 = get("//tmp/t2/@chunk_ids/0")

        assert get("#" + chunk_id1 + "/@ref_counter") == 1
        assert get("#" + chunk_id2 + "/@ref_counter") == 1
        assert_items_equal(get("#" + chunk_id1 + "/@exports"), {})
        assert_items_equal(get("#" + chunk_id2 + "/@exports"), {})
        
        create("table", "//tmp/t", attributes={"external": False})
        merge(mode="ordered",
              in_=["//tmp/t1", "//tmp/t2"],
              out="//tmp/t")

        assert get("//tmp/t/@chunk_ids") == [chunk_id1, chunk_id2]
        assert get("#" + chunk_id1 + "/@ref_counter") == 2
        assert get("#" + chunk_id2 + "/@ref_counter") == 2
        assert_items_equal(get("#" + chunk_id1 + "/@exports"), {"0": {"ref_counter": 1, "vital": True, "replication_factor": 3}})
        assert_items_equal(get("#" + chunk_id2 + "/@exports"), {"0": {"ref_counter": 1, "vital": True, "replication_factor": 3}})
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(0)), [chunk_id1, chunk_id2])
        
        assert read_table("//tmp/t") == [{"a": 1}, {"a": 2}]
        
        remove("//tmp/t")

        gc_collect()
        multicell_sleep()
        assert get("#" + chunk_id1 + "/@ref_counter") == 1
        assert get("#" + chunk_id2 + "/@ref_counter") == 1
        assert_items_equal(get("#" + chunk_id1 + "/@exports"), {})
        assert_items_equal(get("#" + chunk_id2 + "/@exports"), {})
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(0)), [])
        
    @unix_only
    def test_multicell_merge_multi_teleport(self):
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id = get("//tmp/t1/@chunk_ids/0")

        assert get("#" + chunk_id + "/@ref_counter") == 1
        assert_items_equal(get("#" + chunk_id + "/@exports"), {})
        assert not get("#" + chunk_id + "/@foreign")
        assert not exists("#" + chunk_id + "&")
        
        create("table", "//tmp/t2", attributes={"external": False})
        merge(mode="ordered",
              in_=["//tmp/t1", "//tmp/t1"],
              out="//tmp/t2")

        assert get("//tmp/t2/@chunk_ids") == [chunk_id, chunk_id]
        assert get("#" + chunk_id + "/@ref_counter") == 3
        assert_items_equal(get("#" + chunk_id + "/@exports"), {"0": {"ref_counter": 2, "vital": True, "replication_factor": 3}})
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(0)), [chunk_id])
        assert get("#" + chunk_id + "&/@import_ref_counter") == 2
        
        assert read_table("//tmp/t2") == [{"a": 1}, {"a": 1}]
        
        create("table", "//tmp/t3", attributes={"external": False})
        merge(mode="ordered",
              in_=["//tmp/t1", "//tmp/t1"],
              out="//tmp/t3")

        assert get("//tmp/t3/@chunk_ids") == [chunk_id, chunk_id]
        assert get("#" + chunk_id + "/@ref_counter") == 5
        assert_items_equal(get("#" + chunk_id + "/@exports"), {"0": {"ref_counter": 4, "vital": True, "replication_factor": 3}})
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(0)), [chunk_id])
        assert get("#" + chunk_id + "&/@import_ref_counter") == 4

        assert read_table("//tmp/t3") == [{"a": 1}, {"a": 1}]
        
        remove("//tmp/t2")

        gc_collect()
        multicell_sleep()
        assert get("#" + chunk_id + "/@ref_counter") == 5
        assert_items_equal(get("#" + chunk_id + "/@exports"), {"0": {"ref_counter": 4, "vital": True, "replication_factor": 3}})
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(0)), [chunk_id])
        
        remove("//tmp/t3")

        gc_collect()
        multicell_sleep()
        assert get("#" + chunk_id + "/@ref_counter") == 1
        assert_items_equal(get("#" + chunk_id + "/@exports"), {})
        assert_items_equal(ls("//sys/foreign_chunks", driver=get_driver(0)), [])
        
        remove("//tmp/t1")

        gc_collect()
        multicell_sleep()
        assert not exists("#" + chunk_id)

    @unix_only
    def test_multicell_merge_chunk_properties(self):
        create("table", "//tmp/t1", attributes={"replication_factor": 1, "vital": False, "external_cell_tag": 1})
        write_table("//tmp/t1", [{"a": 1}])
        chunk_id = get("//tmp/t1/@chunk_ids/0")

        assert get("#" + chunk_id + "/@replication_factor") == 1
        assert not get("#" + chunk_id + "/@vital")

        create("table", "//tmp/t2", attributes={"replication_factor": 3, "vital": False, "external_cell_tag": 2})
        merge(mode="ordered",
              in_=["//tmp/t1"],
              out="//tmp/t2")

        sleep(0.2)
        assert get("#" + chunk_id + "/@replication_factor") == 3
        assert not get("#" + chunk_id + "/@vital")

        set("//tmp/t2/@replication_factor", 2)

        sleep(0.2)
        assert get("#" + chunk_id + "/@replication_factor") == 2

        set("//tmp/t2/@replication_factor", 3)

        sleep(0.2)
        assert get("#" + chunk_id + "/@replication_factor") == 3

        set("//tmp/t2/@vital", True)

        sleep(0.2)
        assert get("#" + chunk_id + "/@vital")
        
        set("//tmp/t1/@replication_factor", 4)

        sleep(0.2)
        assert get("#" + chunk_id + "/@replication_factor") == 4

        set("//tmp/t1/@replication_factor", 1)

        sleep(0.2)
        assert get("#" + chunk_id + "/@replication_factor") == 3

        remove("//tmp/t2")

        sleep(0.2)
        assert get("#" + chunk_id + "/@replication_factor") == 1
        assert not get("#" + chunk_id + "/@vital")

    @unix_only
    def test_yt_4259(self):
        create("table", "//tmp/t", attributes={"external": False})
        create("table", "//tmp/t1", attributes={"external_cell_tag": 1})
        create("table", "//tmp/t2", attributes={"external_cell_tag": 2})
        
        write_table("//tmp/t", [{"a": 1}])
        chunk_id = get("//tmp/t/@chunk_ids/0")

        merge(mode="ordered", in_=["//tmp/t"], out="//tmp/t1")
        merge(mode="ordered", in_=["//tmp/t"], out="//tmp/t2")

        assert_items_equal(
            get("#" + chunk_id + "/@exports"),
            {
                "1": {"ref_counter": 1, "vital": True, "replication_factor": 3},
                "2": {"ref_counter": 1, "vital": True, "replication_factor": 3}
            })
