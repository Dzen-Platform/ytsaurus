import pytest

from yt_env_setup import YTEnvSetup, unix_only
from yt.environment.helpers import assert_items_equal
from yt_commands import *

from collections import defaultdict


##################################################################

class TestSchedulerMapReduceCommands(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
      "scheduler" : {
        "sort_operation_options" : {
          "min_uncompressed_block_size" : 1
        },
        "map_reduce_operation_options" : {
          "min_uncompressed_block_size" : 1,
          "spec_template" : {
            "use_legacy_controller" : False,
          }
        },
        "enable_partition_map_job_size_adjustment" : True
      }
    }

    def do_run_test(self, method):
        text = \
"""
So, so you think you can tell Heaven from Hell,
blue skies from pain.
Can you tell a green field from a cold steel rail?
A smile from a veil?
Do you think you can tell?
And did they get you to trade your heroes for ghosts?
Hot ashes for trees?
Hot air for a cool breeze?
Cold comfort for change?
And did you exchange a walk on part in the war for a lead role in a cage?
How I wish, how I wish you were here.
We're just two lost souls swimming in a fish bowl, year after year,
Running over the same old ground.
What have you found? The same old fears.
Wish you were here.
"""

        # remove punctuation from text
        stop_symbols = ",.?"
        for s in stop_symbols:
            text = text.replace(s, " ")

        mapper = """
import sys

for line in sys.stdin:
    for word in line.lstrip("line=").split():
        print "word=%s\\tcount=1" % word
"""
        reducer = """
import sys

from itertools import groupby

def read_table():
    for line in sys.stdin:
        row = {}
        fields = line.strip().split("\t")
        for field in fields:
            key, value = field.split("=", 1)
            row[key] = value
        yield row

for key, rows in groupby(read_table(), lambda row: row["word"]):
    count = sum(int(row["count"]) for row in rows)
    print "word=%s\\tcount=%s" % (key, count)
"""

        tx = start_transaction(timeout=30000)

        create("table", "//tmp/t_in", tx=tx)
        create("table", "//tmp/t_map_out", tx=tx)
        create("table", "//tmp/t_reduce_in", tx=tx)
        create("table", "//tmp/t_out", tx=tx)

        for line in text.split("\n"):
            write_table("<append=true>//tmp/t_in", {"line": line}, tx=tx)

        create("file", "//tmp/yt_streaming.py")
        create("file", "//tmp/mapper.py")
        create("file", "//tmp/reducer.py")

        write_file("//tmp/mapper.py", mapper, tx=tx)
        write_file("//tmp/reducer.py", reducer, tx=tx)

        if method == "map_sort_reduce":
            map(in_="//tmp/t_in",
                out="//tmp/t_map_out",
                command="python mapper.py",
                file=["//tmp/mapper.py", "//tmp/yt_streaming.py"],
                spec={"mapper": {"format": "dsv"}},
                tx=tx)

            sort(in_="//tmp/t_map_out",
                 out="//tmp/t_reduce_in",
                 sort_by="word",
                 tx=tx)

            reduce(in_="//tmp/t_reduce_in",
                   out="//tmp/t_out",
                   reduce_by = "word",
                   command="python reducer.py",
                   file=["//tmp/reducer.py", "//tmp/yt_streaming.py"],
                   spec={"reducer": {"format": "dsv"}},
                   tx=tx)
        elif method == "map_reduce":
            map_reduce(in_="//tmp/t_in",
                       out="//tmp/t_out",
                       sort_by="word",
                       mapper_command="python mapper.py",
                       mapper_file=["//tmp/mapper.py", "//tmp/yt_streaming.py"],
                       reduce_combiner_command="python reducer.py",
                       reduce_combiner_file=["//tmp/reducer.py", "//tmp/yt_streaming.py"],
                       reducer_command="python reducer.py",
                       reducer_file=["//tmp/reducer.py", "//tmp/yt_streaming.py"],
                       spec={"partition_count": 2,
                             "map_job_count": 2,
                             "mapper": {"format": "dsv"},
                             "reduce_combiner": {"format": "dsv"},
                             "reducer": {"format": "dsv"},
                             "data_size_per_sort_job": 10},
                       tx=tx)
        elif method == "map_reduce_1p":
            map_reduce(in_="//tmp/t_in",
                       out="//tmp/t_out",
                       sort_by="word",
                       mapper_command="python mapper.py",
                       mapper_file=["//tmp/mapper.py", "//tmp/yt_streaming.py"],
                       reducer_command="python reducer.py",
                       reducer_file=["//tmp/reducer.py", "//tmp/yt_streaming.py"],
                       spec={"partition_count": 1, "mapper": {"format": "dsv"}, "reducer": {"format": "dsv"}},
                       tx=tx)
        elif method == "reduce_combiner_dev_null":
            map_reduce(in_="//tmp/t_in",
                       out="//tmp/t_out",
                       sort_by="word",
                       mapper_command="python mapper.py",
                       mapper_file=["//tmp/mapper.py", "//tmp/yt_streaming.py"],
                       reduce_combiner_command="cat >/dev/null",
                       reducer_command="python reducer.py",
                       reducer_file=["//tmp/reducer.py", "//tmp/yt_streaming.py"],
                       spec={"partition_count": 2,
                             "map_job_count": 2,
                             "mapper": {"format": "dsv"},
                             "reduce_combiner": {"format": "dsv"},
                             "reducer": {"format": "dsv"},
                             "data_size_per_sort_job": 10},
                       tx=tx)
        elif method == "force_reduce_combiners":
            map_reduce(in_="//tmp/t_in",
                       out="//tmp/t_out",
                       sort_by="word",
                       mapper_command="python mapper.py",
                       mapper_file=["//tmp/mapper.py", "//tmp/yt_streaming.py"],
                       reduce_combiner_command="python reducer.py",
                       reduce_combiner_file=["//tmp/reducer.py", "//tmp/yt_streaming.py"],
                       reducer_command="cat",
                       spec={"partition_count": 2,
                             "map_job_count": 2,
                             "mapper": {"format": "dsv"},
                             "reduce_combiner": {"format": "dsv"},
                             "reducer": {"format": "dsv"},
                             "force_reduce_combiners": True},
                       tx=tx)

        commit_transaction(tx)

        # count the desired output
        expected = defaultdict(int)
        for word in text.split():
            expected[word] += 1

        output = []
        if method != "reduce_combiner_dev_null":
            for word, count in expected.items():
                output.append( {"word": word, "count": str(count)} )
            assert_items_equal(read_table("//tmp/t_out"), output)
        else:
            assert_items_equal(read_table("//tmp/t_out"), output)

    @unix_only
    def test_map_sort_reduce(self):
        self.do_run_test("map_sort_reduce")

    @unix_only
    def test_map_reduce(self):
        self.do_run_test("map_reduce")

    @unix_only
    def test_map_reduce_1partition(self):
        self.do_run_test("map_reduce_1p")

    @unix_only
    def test_map_reduce_reduce_combiner_dev_null(self):
        self.do_run_test("reduce_combiner_dev_null")

    @unix_only
    def test_map_reduce_force_reduce_combiners(self):
        self.do_run_test("force_reduce_combiners")

    @unix_only
    def test_many_output_tables(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")
        write_table("//tmp/t_in", {"line": "some_data"})
        map_reduce(in_="//tmp/t_in",
                   out=["//tmp/t_out1", "//tmp/t_out2"],
                   sort_by="line",
                   reducer_command="cat",
                   spec={"reducer": {"format": "dsv"}})

    @unix_only
    def test_reduce_with_sort(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [ {"x": 1, "y" : 2},
                              {"x": 1, "y" : 1},
                              {"x": 1, "y" : 3} ])

        write_table("<append=true>//tmp/t_in",
                            [ {"x": 2, "y" : 3},
                              {"x": 2, "y" : 2},
                              {"x": 2, "y" : 4} ])


        reducer = """
import sys
y = 0
for l in sys.stdin:
  l = l.strip('\\n')
  pairs = l.split('\\t')
  pairs = [a.split("=") for a in pairs]
  d = dict([(a[0], int(a[1])) for a in pairs])
  x = d['x']
  y += d['y']
  print l
print "x={0}\ty={1}".format(x, y)
"""

        create("file", "//tmp/reducer.py")
        write_file("//tmp/reducer.py", reducer)

        map_reduce(in_="//tmp/t_in",
                   out="<sorted_by=[x; y]>//tmp/t_out",
                   reduce_by="x",
                   sort_by=["x", "y"],
                   reducer_file=["//tmp/reducer.py"],
                   reducer_command="python reducer.py",
                   spec={
                     "partition_count": 2,
                     "reducer": {"format": "dsv"}})

        assert read_table("//tmp/t_out") == [{"x": "1", "y" : "1"},
                                       {"x": "1", "y" : "2"},
                                       {"x": "1", "y" : "3"},
                                       {"x": "1", "y" : "6"},
                                       {"x": "2", "y" : "2"},
                                       {"x": "2", "y" : "3"},
                                       {"x": "2", "y" : "4"},
                                       {"x": "2", "y" : "9"}]

        assert get('//tmp/t_out/@sorted')

    @unix_only
    def test_row_count_limit(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [ {"x": 1, "y" : 2}])
        write_table("<append=true>//tmp/t_in",  [ {"x": 2, "y" : 3} ])

        map_reduce(in_="//tmp/t_in",
                   out="<row_count_limit=1>//tmp/t_out",
                   reduce_by="x",
                   sort_by="x",
                   reducer_command="cat",
                   spec={
                     "partition_count": 2,
                     "reducer": {"format": "dsv"},
                     "resource_limits" : { "user_slots" : 1}})

        assert len(read_table("//tmp/t_out")) == 1

    def test_intermediate_live_preview(self):
        create_user("u")
        acl = [make_ace("allow", "u", "write")]

        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        op = map_reduce(dont_track=True, mapper_command="cat", reducer_command="cat; sleep 3",
                        in_="//tmp/t1", out="//tmp/t2",
                        sort_by=["foo"], spec={"intermediate_data_acl": acl})

        time.sleep(2)
        assert exists("//sys/operations/{0}/intermediate".format(op.id))

        intermediate_acl = get("//sys/operations/{0}/intermediate/@acl".format(op.id))
        assert [make_ace("allow", "root", "read")] + acl == intermediate_acl

        op.track()
        assert read_table("//tmp/t2") == [{"foo": "bar"}]

    def test_incorrect_intermediate_data_acl(self):
        create_user("u")
        acl = [make_ace("u", "blabla", "allow")]

        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        with pytest.raises(YtError):
            map_reduce(mapper_command="cat", reducer_command="cat",
                       in_="//tmp/t1", out="//tmp/t2",
                       sort_by=["foo"], spec={"intermediate_data_acl": acl})

    def test_intermediate_compression_codec(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        op = map_reduce(dont_track=True,
                        mapper_command="cat", reducer_command="sleep 5; cat",
                        in_="//tmp/t1", out="//tmp/t2",
                        sort_by=["foo"], spec={"intermediate_compression_codec": "brotli_3"})
        time.sleep(1)
        assert "brotli_3" == get("//sys/operations/{0}/intermediate/@compression_codec".format(op.id))
        op.abort()

    @unix_only
    def test_query_simple(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"a": "b"})

        map_reduce(in_="//tmp/t1", out="//tmp/t2", mapper_command="cat", reducer_command="cat", sort_by=["a"],
            spec={"input_query": "a", "input_schema": [{"name": "a", "type": "string"}]})

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @unix_only
    def test_query_reader_projection(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"a": "b", "c": "d"})

        map_reduce(in_="//tmp/t1", out="//tmp/t2", mapper_command="cat", reducer_command="cat", sort_by=["a"],
            spec={"input_query": "a", "input_schema": [{"name": "a", "type": "string"}]})

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @unix_only
    def test_query_with_condition(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": i} for i in xrange(2)])

        map_reduce(in_="//tmp/t1", out="//tmp/t2", mapper_command="cat", reducer_command="cat", sort_by=["a"],
            spec={"input_query": "a where a > 0", "input_schema": [{"name": "a", "type": "int64"}]})

        assert read_table("//tmp/t2") == [{"a": 1}]

    @unix_only
    def test_query_asterisk(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        rows = [
            {"a": 1, "b": 2, "c": 3},
            {"b": 5, "c": 6},
            {"a": 7, "c": 8}]
        write_table("//tmp/t1", rows)

        schema = [{"name": "z", "type": "int64"},
            {"name": "a", "type": "int64"},
            {"name": "y", "type": "int64"},
            {"name": "b", "type": "int64"},
            {"name": "x", "type": "int64"},
            {"name": "c", "type": "int64"},
            {"name": "u", "type": "int64"}]

        for row in rows:
            for column in schema:
                if column["name"] not in row.keys():
                    row[column["name"]] = None

        map_reduce(in_="//tmp/t1", out="//tmp/t2", mapper_command="cat", reducer_command="cat", sort_by=["a"],
            spec={
                "input_query": "* where a > 0 or b > 0",
                "input_schema": schema})

        assert_items_equal(read_table("//tmp/t2"), rows)

    def test_bad_control_attributes(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        with pytest.raises(YtError):
            map_reduce(mapper_command="cat", reducer_command="cat",
                       in_="//tmp/t1", out="//tmp/t2",
                       sort_by=["foo"],
                       spec={"reduce_job_io": {"control_attributes" : {"enable_table_index" : "true"}}})

    def test_schema_validation(self):
        create("table", "//tmp/input")
        create("table", "//tmp/output", attributes={
            "schema": [
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"}]
            })

        for i in xrange(10):
            write_table("<append=true; sorted_by=[key]>//tmp/input", {"key": i, "value": "foo"})

        map_reduce(
            in_="//tmp/input",
            out="//tmp/output",
            sort_by="key",
            mapper_command="cat",
            reducer_command="cat")

        assert get("//tmp/output/@schema_mode") == "strong"
        assert get("//tmp/output/@schema/@strict")
        assert_items_equal(read_table("//tmp/output"), [{"key": i, "value": "foo"} for i in xrange(10)])

        write_table("<sorted_by=[key]>//tmp/input", {"key": "1", "value": "foo"})

        with pytest.raises(YtError):
            map_reduce(
                in_="//tmp/input",
                out="//tmp/output",
                sort_by="key",
                mapper_command="cat",
                reducer_command="cat")

    def test_computed_columns(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2",
            attributes={
                "schema": [
                    {"name": "k1", "type": "int64", "expression": "k2 * 2" },
                    {"name": "k2", "type": "int64"}]
            })

        write_table("//tmp/t1", [{"k2": i} for i in xrange(2)])

        map_reduce(
            in_="//tmp/t1",
            out="//tmp/t2",
            sort_by="k2",
            mapper_command="cat",
            reducer_command="cat")

        assert get("//tmp/t2/@schema_mode") == "strong"
        assert read_table("//tmp/t2") == [{"k1": i * 2, "k2": i} for i in xrange(2)]

    @unix_only
    def test_map_reduce_input_paths_attr(self):
        create("table", "//tmp/input")
        create("table", "//tmp/output")

        for i in xrange(3):
            write_table("<append=true>//tmp/input", {"key": i, "value": "foo"})

        op = map_reduce(
            dont_track=True,
            in_="//tmp/input",
            out="//tmp/output",
            mapper_command="cat; [ $YT_JOB_INDEX == 0 ] && exit 1 || true",
            reducer_command="cat; exit 2",
            sort_by=["key"],
            spec={
                "max_failed_job_count": 2
            })
        with pytest.raises(YtError):
            op.track();

        jobs_path = "//sys/operations/{0}/jobs".format(op.id)
        job_ids = ls(jobs_path)
        assert len(job_ids) == 2
        for job_id in job_ids:
            job = get("{0}/{1}/@".format(jobs_path, job_id))
            if job["job_type"] == "partition_map":
                expected = yson.loads('[<ranges=[{lower_limit={row_index=0};upper_limit={row_index=3}}]>"//tmp/input"]')
                assert  job["input_paths"] == expected
            else:
                assert job["job_type"] == "partition_reduce"
                assert "input_paths" not in job

    def test_map_reduce_job_size_adjuster_boost(self):
        create("table", "//tmp/t_input")
        # original_data should have at least 1Mb of data
        original_data = [{"index": "%05d" % i, "foo": "a"*35000} for i in xrange(31)]
        for row in original_data:
            write_table("<append=true>//tmp/t_input", row, verbose=False)

        create("table", "//tmp/t_output")

        op = map_reduce(
            in_="//tmp/t_input",
            out="//tmp/t_output",
            sort_by="lines",
            mapper_command="echo lines=`wc -l`",
            reducer_command="cat",
            spec={
                "mapper": {"format": "dsv"},
                "map_job_io": {"table_writer": {"block_size": 1024}},
                "resource_limits": {"user_slots": 1}
            })

        expected = [{"lines": str(2**i)} for i in xrange(5)]
        actual = read_table("//tmp/t_output")
        assert_items_equal(actual, expected)

    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    @pytest.mark.parametrize("sort_order", [None, "ascending"])
    def test_map_reduce_on_dynamic_table(self, sort_order, optimize_for):
        def _create_dynamic_table(path):
            create("table", path,
                attributes = {
                    "schema": [
                        {"name": "key", "type": "int64", "sort_order": sort_order},
                        {"name": "value", "type": "string"}
                    ],
                    "dynamic": True,
                    "optimize_for": optimize_for
                })

        self.sync_create_cells(1)
        _create_dynamic_table("//tmp/t")

        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(6)]
        self.sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)
        self.sync_unmount_table("//tmp/t")

        map_reduce(
            in_="//tmp/t",
            out="//tmp/t_out",
            sort_by="key",
            mapper_command="cat",
            reducer_command="cat")

        assert_items_equal(read_table("//tmp/t_out"), rows)

        rows1 = [{"key": i, "value": str(i+1)} for i in range(3, 10)]
        self.sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows1)
        self.sync_unmount_table("//tmp/t")

        map_reduce(
            in_="//tmp/t",
            out="//tmp/t_out",
            sort_by="key",
            mapper_command="cat",
            reducer_command="cat")

        def update(new):
            def update_row(row):
                if sort_order == "ascending":
                    for r in rows:
                        if r["key"] == row["key"]:
                            r["value"] = row["value"]
                            return
                rows.append(row)
            for row in new:
                update_row(row)

        update(rows1)

        assert_items_equal(read_table("//tmp/t_out"), rows)

##################################################################

class TestSchedulerMapReduceCommandsMulticell(TestSchedulerMapReduceCommands):
    NUM_SECONDARY_MASTER_CELLS = 2
