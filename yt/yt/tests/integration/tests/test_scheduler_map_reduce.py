import pytest

from yt_env_setup import YTEnvSetup, unix_only, wait, parametrize_external
from yt.environment.helpers import assert_items_equal
from yt_commands import *

from collections import defaultdict
from random import shuffle
import datetime

import base64

##################################################################

class TestSchedulerMapReduceCommands(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent" : {
            "sort_operation_options" : {
                "min_uncompressed_block_size" : 1
            },
            "map_reduce_operation_options" : {
                "min_uncompressed_block_size" : 1,
            },
            "enable_partition_map_job_size_adjustment" : True
        }
    }

    @pytest.mark.parametrize("method", ["map_sort_reduce", "map_reduce", "map_reduce_1p", "reduce_combiner_dev_null",
                                        "force_reduce_combiners", "ordered_map_reduce"])
    @authors("ignat")
    def test_simple(self, method):
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

        tx = start_transaction(timeout=60000)

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
        elif method == "ordered_map_reduce":
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
                             "data_size_per_sort_job": 10,
                             "ordered": True},
                       tx=tx)
        else:
            assert False

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

    @authors("ignat")
    @unix_only
    @pytest.mark.parametrize("ordered", [False, True])
    def test_many_output_tables(self, ordered):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")
        write_table("//tmp/t_in", {"line": "some_data"})
        map_reduce(in_="//tmp/t_in",
                   out=["//tmp/t_out1", "//tmp/t_out2"],
                   sort_by="line",
                   reducer_command="cat",
                   spec={"reducer": {"format": "dsv"},
                         "ordered": ordered})

    @authors("psushin")
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

    @authors("babenko")
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

    @authors("levysotsky")
    def test_intermediate_live_preview(self):
        create_user("u")
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        create_user("admin")
        set("//sys/operations/@acl/end", make_ace("allow", "admin", ["read", "write", "manage"]))

        def is_admin_in_base_acl():
            op = map(
                command="cat",
                in_="//tmp/t1",
                out="//tmp/t2",
            )
            return any(ace["subjects"] == ["admin"] for ace in get_operation(op.id)["runtime_parameters"]["acl"])
        wait(is_admin_in_base_acl)

        try:

            op = map_reduce(
                track=False,
                mapper_command="cat",
                reducer_command=with_breakpoint("cat; BREAKPOINT"),
                in_="//tmp/t1",
                out="//tmp/t2",
                sort_by=["foo"],
                spec={
                    "acl": [make_ace("allow", "u", ["read", "manage"])],
                },
            )

            wait(lambda: op.get_job_count("completed") == 1)

            operation_path = op.get_path()
            scheduler_transaction_id = get(operation_path + "/@async_scheduler_transaction_id")
            assert exists(operation_path + "/intermediate", tx=scheduler_transaction_id)

            intermediate_acl = get(operation_path + "/intermediate/@acl", tx=scheduler_transaction_id)
            assert sorted(intermediate_acl) == sorted([
                # "authenticated_user" of operation.
                make_ace("allow", "root", "read"),
                # User from operation ACL.
                make_ace("allow", "u", "read"),
                # User from operation base ACL (from "//sys/operations/@acl").
                make_ace("allow", "admin", "read"),
            ])

            release_breakpoint()
            op.track()
            assert read_table("//tmp/t2") == [{"foo": "bar"}]
        finally:
            remove("//sys/operations/@acl/-1")

    @authors("levysotsky")
    def test_intermediate_new_live_preview(self):
        create_user("admin")
        set("//sys/operations/@acl/end", make_ace("allow", "admin", ["read", "write", "manage"]))
        try:
            create_user("u")
            create("table", "//tmp/t1")
            write_table("//tmp/t1", {"foo": "bar"})
            create("table", "//tmp/t2")

            op = map_reduce(
                track=False,
                mapper_command="cat",
                reducer_command=with_breakpoint("cat; BREAKPOINT"),
                in_="//tmp/t1",
                out="//tmp/t2",
                sort_by=["foo"],
                spec={
                    "acl": [make_ace("allow", "u", ["read"])],
                },
            )

            wait(lambda: op.get_job_count("completed") == 1)

            operation_path = op.get_path()
            get(operation_path + "/controller_orchid/data_flow_graph/vertices")
            intermediate_live_data = read_table(operation_path + "/controller_orchid/data_flow_graph/vertices/partition_map/live_previews/0")

            release_breakpoint()
            op.track()
            assert intermediate_live_data == [{"foo": "bar"}]
            assert read_table("//tmp/t2") == [{"foo": "bar"}]
        finally:
            remove("//sys/operations/@acl/-1")

    @authors("ignat")
    def test_intermediate_compression_codec(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        op = map_reduce(track=False,
                        mapper_command="cat", reducer_command="sleep 5; cat",
                        in_="//tmp/t1", out="//tmp/t2",
                        sort_by=["foo"], spec={"intermediate_compression_codec": "brotli_3"})
        operation_path = op.get_path()
        wait(lambda: exists(operation_path + "/@async_scheduler_transaction_id"))
        async_transaction_id = get(operation_path + "/@async_scheduler_transaction_id")
        wait(lambda: exists(operation_path + "/intermediate", tx=async_transaction_id))
        assert "brotli_3" == get(operation_path + "/intermediate/@compression_codec", tx=async_transaction_id)
        op.abort()

    @authors("savrus")
    @unix_only
    def test_query_simple(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"a": "b"})

        map_reduce(in_="//tmp/t1", out="//tmp/t2", mapper_command="cat", reducer_command="cat", sort_by=["a"],
            spec={"input_query": "a", "input_schema": [{"name": "a", "type": "string"}]})

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @authors("babenko", "dakovalkov")
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_rename_columns_simple(seld, optimize_for):
        create("table", "//tmp/tin", attributes={
                "schema": [{"name": "a", "type": "int64"},
                           {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
            })
        create("table", "//tmp/tout")
        write_table("//tmp/tin", [{"a": 42, "b": 25}])

        map_reduce(in_="<rename_columns={a=b;b=a}>//tmp/tin",
                   out="//tmp/tout",
                   mapper_command="cat",
                   reducer_command="cat",
                   sort_by=["a"])

        assert read_table("//tmp/tout") == [{"b": 42, "a": 25}]

    def _ban_nodes_with_intermediate_chunks(self):
        # Figure out the intermediate chunk
        chunks = ls("//sys/chunks", attributes=["staging_transaction_id"])
        intermediate_chunk_ids = []
        for c in chunks:
            if "staging_transaction_id" in c.attributes:
                tx_id = c.attributes["staging_transaction_id"]
                try:
                    if "Scheduler \"output\" transaction" in get("#{}/@title".format(tx_id)):
                        intermediate_chunk_ids.append(str(c))
                except:
                    # Transaction may vanish
                    pass

        assert len(intermediate_chunk_ids) == 1
        intermediate_chunk_id = intermediate_chunk_ids[0]

        replicas = get("#{}/@stored_replicas".format(intermediate_chunk_id))
        assert len(replicas) == 1
        node_id = replicas[0]

        set("//sys/cluster_nodes/{}/@banned".format(node_id), True)
        return [node_id]

    @authors("psushin")
    @unix_only
    @pytest.mark.parametrize("ordered", [False, True])
    def test_lost_jobs(self, ordered):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"x": 1, "y" : 2}, {"x": 2, "y" : 3}] * 5)

        reducer_cmd = " ; ".join([
            "cat",
            events_on_fs().notify_event_cmd("reducer_started"),
            events_on_fs().wait_event_cmd("continue_reducer")])

        op = map_reduce(in_="//tmp/t_in",
             out="//tmp/t_out",
             reduce_by="x",
             sort_by="x",
             reducer_command=reducer_cmd,
             spec={"partition_count": 2,
                   "sort_locality_timeout" : 0,
                   "sort_assignment_timeout" : 0,
                   "enable_partitioned_data_balancing" : False,
                   "intermediate_data_replication_factor": 1,
                   "sort_job_io" : {"table_reader" : {"retry_count" : 1, "pass_count" : 1}},
                   "resource_limits" : { "user_slots" : 1},
                   "ordered": ordered},
             track=False)

        # We wait for the first reducer to start (second is pending due to resource_limits).
        events_on_fs().wait_event("reducer_started", timeout=datetime.timedelta(1000))

        self._ban_nodes_with_intermediate_chunks()

        # First reducer will probably compelete successfully, but the second one
        # must fail due to unavailable intermediate chunk.
        # This will lead to a lost map job.
        events_on_fs().notify_event("continue_reducer")
        op.track()

        assert get(op.get_path() + "/@progress/partition_jobs/lost") == 1

    @authors("psushin")
    @unix_only
    @pytest.mark.parametrize("ordered", [False, True])
    def test_unavailable_intermediate_chunks(self, ordered):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"x": 1, "y" : 2}, {"x": 2, "y" : 3}] * 5)

        reducer_cmd = " ; ".join([
            "cat",
            events_on_fs().notify_event_cmd("reducer_started"),
            events_on_fs().wait_event_cmd("continue_reducer")])

        op = map_reduce(in_="//tmp/t_in",
             out="//tmp/t_out",
             reduce_by="x",
             sort_by="x",
             reducer_command=reducer_cmd,
             spec={"enable_intermediate_output_recalculation" : False,
                   "sort_assignment_timeout" : 0,
                   "sort_locality_timeout" : 0,
                   "enable_partitioned_data_balancing" : False,
                   "intermediate_data_replication_factor": 1,
                   "sort_job_io" : {"table_reader" : {"retry_count" : 1, "pass_count" : 1}},
                   "partition_count": 2,
                   "resource_limits" : { "user_slots" : 1},
                   "ordered": ordered},
             track=False)

        # We wait for the first reducer to start (the second one is pending due to resource_limits).
        events_on_fs().wait_event("reducer_started", timeout=datetime.timedelta(1000))

        banned_nodes = self._ban_nodes_with_intermediate_chunks()

        # The first reducer will probably complete successfully, but the second one
        # must fail due to unavailable intermediate chunk.
        # This will lead to a lost map job.
        events_on_fs().notify_event("continue_reducer")

        def get_unavailable_chunk_count():
            return get(op.get_path() + "/@progress/estimated_input_statistics/unavailable_chunk_count")

        # Wait till scheduler discovers that chunk is unavailable.
        wait(lambda: get_unavailable_chunk_count() > 0)

        # Make chunk available again.
        for n in banned_nodes:
            set("//sys/cluster_nodes/{0}/@banned".format(n), False)

        wait(lambda: get_unavailable_chunk_count() == 0)

        op.track()

        assert get(op.get_path() + "/@progress/partition_reduce_jobs/aborted") > 0
        assert get(op.get_path() + "/@progress/partition_jobs/lost") == 0

    @authors("max42")
    @pytest.mark.parametrize("ordered", [False, True])
    def test_progress_counter(self, ordered):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")

        write_table("//tmp/t_in", [{"x": 1, "y" : 2}])

        reducer_cmd = " ; ".join([
            "cat",
            events_on_fs().notify_event_cmd("reducer_started"),
            events_on_fs().wait_event_cmd("continue_reducer")])

        op = map_reduce(in_="//tmp/t_in",
                        out="//tmp/t_out",
                        reduce_by="x",
                        sort_by="x",
                        reducer_command=reducer_cmd,
                        spec={"partition_count": 1, "ordered": ordered},
                        track=False)

        events_on_fs().wait_event("reducer_started", timeout=datetime.timedelta(1000))

        job_ids = list(op.get_running_jobs())
        assert len(job_ids) == 1
        job_id = job_ids[0]

        abort_job(job_id)

        events_on_fs().notify_event("continue_reducer")

        op.track()

        partition_reduce_counter = get(op.get_path() + "/@progress/data_flow_graph/vertices/partition_reduce/job_counter")
        total_counter = get(op.get_path() + "/@progress/jobs")

        assert partition_reduce_counter["aborted"]["total"] == 1
        assert partition_reduce_counter["pending"] == 0

    @authors("savrus")
    @unix_only
    def test_query_reader_projection(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"a": "b", "c": "d"})

        map_reduce(in_="//tmp/t1", out="//tmp/t2", mapper_command="cat", reducer_command="cat", sort_by=["a"],
            spec={"input_query": "a", "input_schema": [{"name": "a", "type": "string"}]})

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @authors("savrus")
    @unix_only
    def test_query_with_condition(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": i} for i in xrange(2)])

        map_reduce(in_="//tmp/t1", out="//tmp/t2", mapper_command="cat", reducer_command="cat", sort_by=["a"],
            spec={"input_query": "a where a > 0", "input_schema": [{"name": "a", "type": "int64"}]})

        assert read_table("//tmp/t2") == [{"a": 1}]

    @authors("savrus", "psushin")
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

    @authors("babenko", "ignat", "klyachin")
    def test_bad_control_attributes(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        with pytest.raises(YtError):
            map_reduce(mapper_command="cat", reducer_command="cat",
                       in_="//tmp/t1", out="//tmp/t2",
                       sort_by=["foo"],
                       spec={"reduce_job_io": {"control_attributes" : {"enable_table_index" : "true"}}})

    @authors("savrus")
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

    @authors("savrus")
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

    @authors("klyachin")
    @pytest.mark.skipif("True", reason="YT-8228")
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

    @authors("max42")
    @pytest.mark.parametrize("sorted", [False, True])
    @pytest.mark.parametrize("ordered", [False, True])
    def test_map_output_table(self, sorted, ordered):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")
        create("table", "//tmp/t_out_map", attributes={
            "schema": [
                {"name": "bypass_key", "type": "int64", "sort_order": "ascending" if sorted else None}
            ]
        })

        write_table("<append=%true>//tmp/t_in", [{"a": i} for i in range(10)])

        op = map_reduce(
            in_="//tmp/t_in",
            out=["//tmp/t_out_map", "//tmp/t_out"],
            mapper_command="echo \"{bypass_key=$YT_JOB_INDEX}\" 1>&4; echo '{shuffle_key=23}'",
            reducer_command="cat",
            reduce_by=["shuffle_key"],
            sort_by=["shuffle_key"],
            spec={"mapper_output_table_count" : 1, "max_failed_job_count": 1, "data_size_per_map_job": 1, "ordered": ordered})
        assert read_table("//tmp/t_out") == [{"shuffle_key": 23}] * 10
        assert len(read_table("//tmp/t_out_map")) == 10

    @authors("max42")
    def test_data_balancing(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        job_count = 20
        node_count = get("//sys/cluster_nodes/@count")
        write_table("//tmp/t1", [{"a": "x" * 10**6} for i in range(job_count)])
        op = map_reduce(in_="//tmp/t1",
                        out="//tmp/t2",
                        spec={"min_locality_input_data_weight": 1,
                              "enable_partitioned_data_balancing": True,
                              "data_size_per_map_job": 1,
                              "mapper": {"format": "dsv"},
                              "reducer": {"format": "dsv"}},
                        sort_by=["cwd"],
                        mapper_command="echo cwd=`pwd`",
                        reducer_command="cat")

        cnt = {}
        for row in read_table("//tmp/t2"):
            cnt[row["cwd"]] = cnt.get(row["cwd"], 0) + 1
        values = cnt.values()
        print_debug(values)
        assert max(values) <= 2 * job_count // node_count

    @authors("dakovalkov")
    def test_ordered_map_reduce(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")
        for i in range(50):
            write_table("<append=%true>//tmp/t_in", [{"key": i}])
        op = map_reduce(in_="//tmp/t_in",
                        out="//tmp/t_out",
                        mapper_command="cat",
                        reducer_command="cat",
                        sort_by=["key"],
                        map_job_count=1,
                        ordered=True)

        assert read_table("//tmp/t_in") == read_table("//tmp/t_out")

    @authors("babenko")
    def test_commandless_user_job_spec(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")
        for i in range(50):
            write_table("<append=%true>//tmp/t_in", [{"key": i}])
        op = map_reduce(in_="//tmp/t_in",
                        out="//tmp/t_out",
                        reducer_command="cat",
                        sort_by=["key"],
                        spec={
                            "mapper": {"cpu_limit": 1},
                            "reduce_combiner": {"cpu_limit": 1}
                        })

        assert_items_equal(read_table("//tmp/t_in"), read_table("//tmp/t_out"))

    @authors("max42")
    def test_sampling(self):
        create("table", "//tmp/t1", attributes={"schema": [{"name": "key", "type": "string"},
                                                           {"name": "value", "type": "string"}]})
        create("table", "//tmp/t2") # This table is used for job counting.
        create("table", "//tmp/t3") # This table will contain the cat'ed input.
        write_table("//tmp/t1",
                    [{"key": ("%02d" % (i // 100)), "value": "x" * 10**2} for i in range(10000)],
                    table_writer={"block_size": 1024})

        map_reduce(in_="//tmp/t1",
                   out=["//tmp/t2", "//tmp/t3"],
                   mapper_command="cat; echo '{a=1}' >&4",
                   reducer_command="cat",
                   sort_by=["key"],
                   spec={"sampling": {"sampling_rate": 0.5, "io_block_size": 10**5},
                         "mapper_output_table_count": 1})
        assert get("//tmp/t2/@row_count") == 1
        assert 0.25 * 10000 <= get("//tmp/t3/@row_count") <= 0.75 * 10000

        map_reduce(in_="//tmp/t1",
                   out=["//tmp/t2", "//tmp/t3"],
                   mapper_command="cat; echo '{a=1}' >&4",
                   reducer_command="cat",
                   sort_by=["key"],
                   spec={"sampling": {"sampling_rate": 0.5, "io_block_size": 10**5},
                         "map_job_count": 10,
                         "mapper_output_table_count": 1})
        assert get("//tmp/t2/@row_count") > 1
        assert 0.25 * 10000 <= get("//tmp/t3/@row_count") <= 0.75 * 10000

    @authors("gritukan")
    def test_pivot_keys(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        rows = [{"key": "%02d" % key} for key in range(50)]
        shuffle(rows)
        write_table("//tmp/t1", rows)

        map_reduce(in_="//tmp/t1",
                   out="//tmp/t2",
                   mapper_command="cat",
                   reducer_command="cat",
                   sort_by=["key"],
                   spec={"pivot_keys": [["01"], ["43"]]})

        assert_items_equal(read_table("//tmp/t2"), sorted(rows))
        chunk_ids = get("//tmp/t2/@chunk_ids")
        assert sorted([get("#" + chunk_id + "/@row_count") for chunk_id in chunk_ids]) == [1, 7, 42]

        map_reduce(in_="//tmp/t1",
                   out="//tmp/t3",
                   reducer_command="cat",
                   sort_by=["key"],
                   spec={"pivot_keys": [["01"], ["43"]]})

        assert_items_equal(read_table("//tmp/t3"), sorted(rows))
        chunk_ids = get("//tmp/t3/@chunk_ids")
        assert sorted([get("#" + chunk_id + "/@row_count") for chunk_id in chunk_ids]) == [1, 7, 42]

    @authors("gritukan")
    def test_pivot_keys_incorrect_options(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        rows = [{"key": "%02d" % key} for key in range(50)]
        shuffle(rows)
        write_table("//tmp/t1", rows)

        with pytest.raises(YtError):
            map_reduce(in_="//tmp/t1",
                       out="//tmp/t2",
                       mapper_command="cat",
                       reducer_command="cat",
                       sort_by=["key"],
                       spec={"pivot_keys": [["73"], ["37"]]})

##################################################################

class TestSchedulerMapReduceCommandsMulticell(TestSchedulerMapReduceCommands):
    NUM_SECONDARY_MASTER_CELLS = 2

class TestSchedulerMapReduceCommandsPortal(TestSchedulerMapReduceCommandsMulticell):
    ENABLE_TMP_PORTAL = True

##################################################################
