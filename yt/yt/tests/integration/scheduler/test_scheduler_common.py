from yt_env_setup import (
    YTEnvSetup,
    Restarter,
    SCHEDULERS_SERVICE,
    CONTROLLER_AGENTS_SERVICE,
)
from yt_commands import (
    authors, create_test_tables, extract_statistic_v2, extract_deprecated_statistic, print_debug, wait, wait_breakpoint, release_breakpoint, with_breakpoint, create,
    ls, get,
    set, copy, move, remove, exists, concatenate, create_user, create_pool, create_pool_tree, make_ace,
    add_member, start_transaction, abort_transaction,
    read_table, write_table, read_file,
    map, reduce,
    map_reduce, merge, sort, get_job,
    run_test_vanilla, run_sleeping_vanilla, get_job_fail_context, dump_job_context,
    get_singular_chunk_id, PrepareTables,
    raises_yt_error, update_scheduler_config, update_controller_agent_config,
    assert_statistics, sorted_dicts,
    set_banned_flag)

from yt_type_helpers import make_schema

from yt_scheduler_helpers import scheduler_orchid_default_pool_tree_config_path, scheduler_orchid_path

from yt_helpers import profiler_factory, read_structured_log, write_log_barrier

import yt.yson as yson

from yt.wrapper import JsonFormat
from yt.common import date_string_to_timestamp, YtError

import pytest

from collections import defaultdict
import io
import os
import time
try:
    import zstd
except ImportError:
    import zstandard as zstd

import builtins


##################################################################

def get_by_composite_key(item, composite_key, default=None):
    current_item = item
    for part in composite_key:
        if part not in current_item:
            return default
        else:
            current_item = current_item[part]
    return current_item

##################################################################


SCHEDULER_COMMON_NODE_CONFIG_PATCH = {
    "exec_agent": {
        "job_controller": {
            "resource_limits": {
                "user_slots": 5,
                "cpu": 5,
                "memory": 5 * 1024 ** 3,
            }
        }
    }
}


class TestSchedulerCommon(YTEnvSetup):
    NUM_TEST_PARTITIONS = 4
    NUM_MASTERS = 1
    NUM_NODES = 3
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
            "snapshot_period": 500,
            "operations_update_period": 10,
            "map_operation_options": {
                "job_splitter": {
                    "min_job_time": 5000,
                    "min_total_data_size": 1024,
                    "update_period": 100,
                    "candidate_percentile": 0.8,
                    "max_jobs_per_split": 3,
                },
            },
            "controller_throttling_log_backoff": 0,
        }
    }

    DELTA_NODE_CONFIG = SCHEDULER_COMMON_NODE_CONFIG_PATCH
    USE_PORTO = True

    @authors("ignat")
    def test_failed_jobs_twice(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"foo": "bar"} for _ in range(200)])

        op = map(
            track=False,
            in_="//tmp/t1",
            out="//tmp/t2",
            command='trap "" HUP; bash -c "sleep 60" &; sleep $[( $RANDOM % 5 )]s; exit 42;',
            spec={"max_failed_job_count": 1, "job_count": 200},
        )

        with pytest.raises(YtError):
            op.track()

        for job_id in op.list_jobs():
            job_desc = get_job(op.id, job_id)
            if job_desc["state"] == "running":
                continue
            assert "Process exited with code " in job_desc["error"]["inner_errors"][0]["message"]

    @authors("ignat")
    def test_job_progress(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"foo": "bar"} for _ in range(10)])

        op = map(
            track=False,
            label="job_progress",
            in_="//tmp/t1",
            out="//tmp/t2",
            command=with_breakpoint("cat ; BREAKPOINT"),
            spec={"test_flag": yson.to_yson_type("value", attributes={"attr": 0})},
        )

        jobs = wait_breakpoint()
        progress = get(op.get_path() + "/controller_orchid/running_jobs/" + jobs[0] + "/progress")
        assert progress >= 0

        test_flag = get("//sys/scheduler/orchid/scheduler/operations/{0}/spec/test_flag".format(op.id))
        assert str(test_flag) == "value"
        assert test_flag.attributes == {"attr": 0}

        release_breakpoint()
        op.track()

    @authors("ermolovd")
    def test_job_stderr_size(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"foo": "bar"} for _ in range(10)])

        op = map(
            track=False,
            label="job_progress",
            in_="//tmp/t1",
            out="//tmp/t2",
            command=with_breakpoint("echo FOOBAR >&2 ; BREAKPOINT; cat"),
        )

        jobs = wait_breakpoint()

        def get_stderr_size():
            return get(op.get_path() + "/controller_orchid/running_jobs/" + jobs[0] + "/stderr_size")

        wait(lambda: get_stderr_size() == len("FOOBAR\n"))

        release_breakpoint()
        op.track()

    @authors("ignat")
    def test_estimated_statistics(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"key": i} for i in range(5)])

        sort(in_="//tmp/t1", out="//tmp/t1", sort_by="key")
        op = map(command="cat", in_="//tmp/t1[:1]", out="//tmp/t2")

        statistics = get(op.get_path() + "/@progress/estimated_input_statistics")
        for key in [
            "uncompressed_data_size",
            "compressed_data_size",
            "row_count",
            "data_weight",
        ]:
            assert statistics[key] > 0
        assert statistics["unavailable_chunk_count"] == 0
        assert statistics["chunk_count"] == 1

    @authors("ignat")
    def test_invalid_output_record(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"key": "foo", "value": "ninja"})

        command = """awk '($1=="foo"){print "bar"}'"""

        with pytest.raises(YtError):
            map(
                in_="//tmp/t1",
                out="//tmp/t2",
                command=command,
                spec={"mapper": {"format": "yamr"}},
            )

    @authors("ignat")
    def test_fail_context(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"foo": "bar"})

        op = map(
            track=False,
            in_="//tmp/t1",
            out="//tmp/t2",
            command='python -c "import os; os.read(0, 1);"',
            spec={"mapper": {"input_format": "dsv", "check_input_fully_consumed": True}, "max_failed_job_count": 2},
        )

        # If all jobs failed then operation is also failed
        with pytest.raises(YtError):
            op.track()

        for job_id in op.list_jobs():
            fail_context = get_job_fail_context(op.id, job_id)
            assert len(fail_context) > 0

    @authors("ignat")
    def test_dump_job_context(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"foo": "bar"})

        op = map(
            track=False,
            label="dump_job_context",
            in_="//tmp/t1",
            out="//tmp/t2",
            command=with_breakpoint("cat ; BREAKPOINT"),
            spec={"mapper": {"input_format": "json", "output_format": "json"}},
        )

        jobs = wait_breakpoint()
        # Wait till job starts reading input
        wait(lambda: get(op.get_path() + "/controller_orchid/running_jobs/" + jobs[0] + "/progress") >= 0.5)

        dump_job_context(jobs[0], "//tmp/input_context")

        release_breakpoint()
        op.track()

        context = read_file("//tmp/input_context")
        assert get("//tmp/input_context/@description/type") == "input_context"
        assert JsonFormat().loads_row(context)["foo"] == "bar"

    @authors("ignat")
    def test_dump_job_context_permissions(self):
        create_user("abc")
        create(
            "map_node",
            "//tmp/dir",
            attributes={"acl": [{"action": "deny", "subjects": ["abc"], "permissions": ["write"]}]},
        )

        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", {"foo": "bar"})

        op = map(
            track=False,
            label="dump_job_context",
            in_="//tmp/t1",
            out="//tmp/t2",
            command=with_breakpoint("cat ; BREAKPOINT"),
            spec={"mapper": {"input_format": "json", "output_format": "json"}},
            authenticated_user="abc",
        )

        jobs = wait_breakpoint()
        # Wait till job starts reading input
        wait(lambda: get(op.get_path() + "/controller_orchid/running_jobs/" + jobs[0] + "/progress") >= 0.5)

        with pytest.raises(YtError):
            dump_job_context(jobs[0], "//tmp/dir/input_context", authenticated_user="abc")

        assert not exists("//tmp/dir/input_context")

        release_breakpoint()
        op.track()

    @authors("ignat")
    def test_large_spec(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", [{"a": "b"}])

        with pytest.raises(YtError):
            map(
                in_="//tmp/t1",
                out="//tmp/t2",
                command="cat",
                spec={"attribute": "really_large" * (2 * 10 ** 6)},
                verbose=False,
            )

    @authors("ignat")
    def test_job_with_exit_immediately_flag(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output")
        write_table("//tmp/t_input", {"foo": "bar"})

        op = map(
            track=False,
            in_="//tmp/t_input",
            out="//tmp/t_output",
            command="set -e; /non_existed_command; echo stderr >&2;",
            spec={"max_failed_job_count": 1},
        )

        with pytest.raises(YtError):
            op.track()

        job_ids = op.list_jobs()
        assert len(job_ids) == 1
        assert op.read_stderr(job_ids[0]) == b"/bin/bash: /non_existed_command: No such file or directory\n"

    @authors("ignat")
    def test_pipe_statistics(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output")
        write_table("//tmp/t_input", {"foo": "bar"})

        op = map(command="cat", in_="//tmp/t_input", out="//tmp/t_output")

        assert_statistics(
            op,
            "user_job.pipes.input.bytes",
            lambda bytes: bytes == 15)
        assert_statistics(
            op,
            "user_job.pipes.output.0.bytes",
            lambda bytes: bytes == 15)

    @authors("ignat")
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

        write_table("//tmp/t_in", [{"value": "A" * 1024} for _ in range(10)])

        map(command="cat", in_="//tmp/t_in", out="//tmp/t_out", spec={"job_count": 1})

        chunk_id = get_singular_chunk_id("//tmp/t_out")
        assert get("#" + chunk_id + "/@compressed_data_size") > 1024 * 10
        assert get("#" + chunk_id + "/@max_block_size") < 1024 * 2

    @authors("ignat")
    def test_invalid_schema_in_path(self):
        create("table", "//tmp/input")
        create("table", "//tmp/output")

        with pytest.raises(YtError):
            map(
                in_="//tmp/input",
                out="<schema=[{name=key; type=int64}; {name=key;type=string}]>//tmp/output",
                command="cat",
            )

    @authors("ignat")
    def test_ypath_attributes_on_output_tables(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"a": "b" * 10000})

        for optimize_for in ["lookup", "scan"]:
            create("table", "//tmp/tout1_" + optimize_for)
            map(
                in_="//tmp/t1",
                out="<optimize_for={0}>//tmp/tout1_{0}".format(optimize_for),
                command="cat",
            )
            assert get("//tmp/tout1_{}/@optimize_for".format(optimize_for)) == optimize_for

        for compression_codec in ["none", "lz4"]:
            create("table", "//tmp/tout2_" + compression_codec)
            map(
                in_="//tmp/t1",
                out="<compression_codec={0}>//tmp/tout2_{0}".format(compression_codec),
                command="cat",
            )

            stats = get("//tmp/tout2_{}/@compression_statistics".format(compression_codec))
            assert compression_codec in stats, str(stats)
            assert stats[compression_codec]["chunk_count"] > 0

    @authors("ignat")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_unique_keys_validation(self, optimize_for):
        create("table", "//tmp/t1")
        create(
            "table",
            "//tmp/t2",
            attributes={
                "optimize_for": optimize_for,
                "schema": make_schema(
                    [
                        {"name": "key", "type": "int64", "sort_order": "ascending"},
                        {"name": "value", "type": "string"},
                    ],
                    unique_keys=True,
                ),
            },
        )

        for i in range(2):
            write_table("<append=true>//tmp/t1", {"key": "foo", "value": "ninja"})

        command = 'cat >/dev/null; echo "{key=1; value=one}"'

        with pytest.raises(YtError):
            map(in_="//tmp/t1", out="//tmp/t2", command=command, spec={"job_count": 2})

        command = 'cat >/dev/null; echo "{key=1; value=one}; {key=1; value=two}"'

        with pytest.raises(YtError):
            map(in_="//tmp/t1", out="//tmp/t2", command=command, spec={"job_count": 1})

    @authors("dakovalkov")
    def test_append_to_sorted_table_simple(self):
        create(
            "table",
            "//tmp/sorted_table",
            attributes={
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    unique_keys=False,
                )
            },
        )
        write_table("//tmp/sorted_table", [{"key": 1}, {"key": 5}, {"key": 10}])
        map(
            in_="//tmp/sorted_table",
            out="<append=%true>//tmp/sorted_table",
            command="echo '{key=30};{key=39}'",
            spec={"job_count": 1},
        )

        assert read_table("//tmp/sorted_table") == [
            {"key": 1},
            {"key": 5},
            {"key": 10},
            {"key": 30},
            {"key": 39},
        ]

    @authors("dakovalkov")
    def test_append_to_sorted_table_failed(self):
        create(
            "table",
            "//tmp/sorted_table",
            attributes={
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    unique_keys=False,
                )
            },
        )
        write_table("//tmp/sorted_table", [{"key": 1}, {"key": 5}, {"key": 10}])

        with pytest.raises(YtError):
            map(
                in_="//tmp/sorted_table",
                out="<append=%true>//tmp/sorted_table",
                command="echo '{key=7};{key=39}'",
                spec={"job_count": 1},
            )

    @authors("dakovalkov")
    def test_append_to_sorted_table_unique_keys(self):
        create(
            "table",
            "//tmp/sorted_table",
            attributes={
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    unique_keys=False,
                )
            },
        )
        write_table("//tmp/sorted_table", [{"key": 1}, {"key": 5}, {"key": 10}])
        map(
            in_="//tmp/sorted_table",
            out="<append=%true>//tmp/sorted_table",
            command="echo '{key=10};{key=39}'",
            spec={"job_count": 1},
        )

        assert read_table("//tmp/sorted_table") == [
            {"key": 1},
            {"key": 5},
            {"key": 10},
            {"key": 10},
            {"key": 39},
        ]

    @authors("dakovalkov")
    def test_append_to_sorted_table_unique_keys_failed(self):
        create(
            "table",
            "//tmp/sorted_table",
            attributes={
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    unique_keys=True,
                )
            },
        )
        write_table("//tmp/sorted_table", [{"key": 1}, {"key": 5}, {"key": 10}])

        with pytest.raises(YtError):
            map(
                in_="//tmp/sorted_table",
                out="<append=%true>//tmp/sorted_table",
                command="echo '{key=10};{key=39}'",
                spec={"job_count": 1},
            )

    @authors("dakovalkov")
    def test_append_to_sorted_table_empty_table(self):
        create(
            "table",
            "//tmp/sorted_table",
            attributes={
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    unique_keys=False,
                )
            },
        )
        create("table", "//tmp/t1")
        write_table("//tmp/t1", [{}])
        map(
            in_="//tmp/t1",
            out="<append=%true>//tmp/sorted_table",
            command="echo '{key=30};{key=39}'",
            spec={"job_count": 1},
        )

        assert read_table("//tmp/sorted_table") == [{"key": 30}, {"key": 39}]

    @authors("dakovalkov")
    def test_append_to_sorted_table_empty_row_empty_table(self):
        create(
            "table",
            "//tmp/sorted_table",
            attributes={
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    unique_keys=False,
                )
            },
        )
        create("table", "//tmp/t1")
        write_table("//tmp/t1", [{}])
        map(
            in_="//tmp/t1",
            out="<append=%true>//tmp/sorted_table",
            command="echo '{ }'",
            spec={"job_count": 1},
        )

        assert read_table("//tmp/sorted_table") == [{"key": yson.YsonEntity()}]

    @authors("dakovalkov")
    def test_append_to_sorted_table_exclusive_lock(self):
        create(
            "table",
            "//tmp/sorted_table",
            attributes={
                "schema": make_schema(
                    [{"name": "key", "type": "int64", "sort_order": "ascending"}],
                    unique_keys=False,
                )
            },
        )
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/sorted_table", [{"key": 5}])
        write_table("//tmp/t1", [{"key": 6}, {"key": 10}])
        write_table("//tmp/t2", [{"key": 8}, {"key": 12}])

        map(
            track=False,
            in_="//tmp/t1",
            out="<append=%true>//tmp/sorted_table",
            command="sleep 10; cat",
        )

        time.sleep(5)

        with pytest.raises(YtError):
            map(in_="//tmp/t2", out="<append=%true>//tmp/sorted_table", command="cat")

    @authors("ignat")
    @pytest.mark.timeout(150)
    def test_many_parallel_operations(self):
        create("table", "//tmp/input")

        testing_options = {"controller_scheduling_delay": {"duration": 100}}

        job_count = 20
        original_data = [{"index": i} for i in range(job_count)]
        write_table("//tmp/input", original_data)

        operation_count = 5
        ops = []
        for index in range(operation_count):
            output = "//tmp/output" + str(index)
            create("table", output)
            ops.append(
                map(
                    in_="//tmp/input",
                    out=[output],
                    command="sleep 0.1; cat",
                    spec={"data_size_per_job": 1, "testing": testing_options},
                    track=False,
                )
            )

        failed_ops = []
        for index in range(operation_count):
            output = "//tmp/failed_output" + str(index)
            create("table", output)
            failed_ops.append(
                map(
                    in_="//tmp/input",
                    out=[output],
                    command="sleep 0.1; exit 1",
                    spec={
                        "data_size_per_job": 1,
                        "max_failed_job_count": 1,
                        "testing": testing_options,
                    },
                    track=False,
                )
            )

        for index, op in enumerate(failed_ops):
            "//tmp/failed_output" + str(index)
            with pytest.raises(YtError):
                op.track()

        for index, op in enumerate(ops):
            output = "//tmp/output" + str(index)
            op.track()
            assert sorted_dicts(read_table(output)) == original_data

        time.sleep(5)
        statistics = get("//sys/scheduler/orchid/monitoring/ref_counted/statistics")
        operation_objects = [
            "NYT::NScheduler::TOperation",
            "NYT::NScheduler::TSchedulerOperationElement",
        ]
        records = [record for record in statistics if record["name"] in operation_objects]
        assert len(records) == 2
        assert records[0]["objects_alive"] == 0
        assert records[1]["objects_alive"] == 0

    @authors("ignat")
    def test_concurrent_fail(self):
        create("table", "//tmp/input")

        testing_options = {"controller_scheduling_delay": {"duration": 250}}

        job_count = 1000
        original_data = [{"index": i} for i in range(job_count)]
        write_table("//tmp/input", original_data)

        create("table", "//tmp/output")
        with pytest.raises(YtError):
            map(
                in_="//tmp/input",
                out="//tmp/output",
                command="sleep 0.250; exit 1",
                spec={
                    "data_size_per_job": 1,
                    "max_failed_job_count": 10,
                    "testing": testing_options,
                },
            )

    @authors("ignat")
    def test_YT_5629(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")

        data = [{"a": i} for i in range(5)]
        write_table("//tmp/t1", data)

        map(in_="//tmp/t1", out="//tmp/t2", command="sleep 1; cat /proc/self/fd/0")

        assert read_table("//tmp/t2") == data

    @authors("ignat")
    def test_range_count_limit(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", {"key": "a", "value": "value"})

        def gen_table(range_count):
            return "<ranges=[" + ("{exact={row_index=0}};" * range_count) + "]>//tmp/in"

        map(in_=[gen_table(20)], out="//tmp/out", command="cat")

        with pytest.raises(YtError):
            map(in_=[gen_table(2000)], out="//tmp/out", command="cat")

    @authors("ignat")
    def test_complete_op(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        for i in range(5):
            write_table("<append=true>//tmp/t1", {"key": str(i), "value": "foo"})

        op = map(
            track=False,
            in_="//tmp/t1",
            out="//tmp/t2",
            command=with_breakpoint("echo job_index=$YT_JOB_INDEX ; BREAKPOINT"),
            spec={
                "mapper": {"format": "dsv"},
                "data_size_per_job": 1,
                "max_failed_job_count": 1,
            },
        )
        jobs = wait_breakpoint(job_count=5)

        for job_id in jobs[:3]:
            release_breakpoint(job_id=job_id)

        assert op.get_state() != "completed"
        wait(lambda: op.get_job_count("completed") >= 3)

        op.complete()
        assert op.get_state() == "completed"
        op.track()
        assert len(read_table("//tmp/t2")) == 3
        assert "operation_completed_by_user_request" in op.get_alerts()

    @authors("ignat")
    def test_abort_op(self):
        create("table", "//tmp/t")
        write_table("//tmp/t", {"foo": "bar"})

        op = map(track=False, in_="//tmp/t", out="//tmp/t", command="sleep 1")

        op.abort()
        assert op.get_state() == "aborted"

    @authors("ignat")
    def test_input_with_custom_transaction(self):
        custom_tx = start_transaction(timeout=30000)

        create("table", "//tmp/in", tx=custom_tx)
        write_table("//tmp/in", {"foo": "bar"}, tx=custom_tx)

        create("table", "//tmp/out")

        with pytest.raises(YtError):
            map(command="cat", in_="//tmp/in", out="//tmp/out")

        map(
            command="cat",
            in_='<transaction_id="{}">//tmp/in'.format(custom_tx),
            out="//tmp/out",
        )

        assert list(read_table("//tmp/out")) == [{"foo": "bar"}]

    @authors("babenko")
    def test_input_created_in_user_transaction(self):
        custom_tx = start_transaction()
        create("table", "//tmp/in", tx=custom_tx)
        write_table("//tmp/in", {"foo": "bar"}, tx=custom_tx)
        create("table", "//tmp/out")
        with pytest.raises(YtError):
            map(command="cat", in_="//tmp/in", out="//tmp/out")

    @authors("ignat")
    def test_nested_input_transactions(self):
        custom_tx = start_transaction(timeout=60000)

        create("table", "//tmp/in", tx=custom_tx)
        write_table("//tmp/in", {"foo": "bar"}, tx=custom_tx)

        create("table", "//tmp/out")

        op = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 100"),
            in_='<transaction_id="{}">//tmp/in'.format(custom_tx),
            out="//tmp/out",
        )

        wait_breakpoint()

        nested_input_transaction_ids = get(op.get_path() + "/@nested_input_transaction_ids")
        assert len(nested_input_transaction_ids) == 1
        nested_tx = nested_input_transaction_ids[0]

        assert list(read_table("//tmp/in", tx=nested_tx)) == [{"foo": "bar"}]
        assert get("#{}/@parent_id".format(nested_tx)) == custom_tx

        op.wait_for_fresh_snapshot()

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        op.ensure_running()
        assert get(op.get_path() + "/@nested_input_transaction_ids") == [nested_tx]

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            abort_transaction(nested_tx)

        op.ensure_running()
        new_nested_input_transaction_ids = get(op.get_path() + "/@nested_input_transaction_ids")
        assert len(new_nested_input_transaction_ids) == 1
        assert new_nested_input_transaction_ids[0] != nested_tx

    @authors("ignat")
    def test_nested_input_transaction_duplicates(self):
        custom_tx = start_transaction(timeout=60000)

        create("table", "//tmp/in", tx=custom_tx)
        write_table("//tmp/in", {"foo": "bar"}, tx=custom_tx)

        create("table", "//tmp/out")

        op = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 100"),
            in_=['<transaction_id="{}">//tmp/in'.format(custom_tx)] * 2,
            out="//tmp/out",
        )

        wait_breakpoint()

        nested_input_transaction_ids = get(op.get_path() + "/@nested_input_transaction_ids")
        assert len(nested_input_transaction_ids) == 2
        assert nested_input_transaction_ids[0] == nested_input_transaction_ids[1]

        nested_tx = nested_input_transaction_ids[0]
        assert list(read_table("//tmp/in", tx=nested_tx)) == [{"foo": "bar"}]
        assert get("#{}/@parent_id".format(nested_tx)) == custom_tx

        op.wait_for_fresh_snapshot()

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        op.ensure_running()
        assert get(op.get_path() + "/@nested_input_transaction_ids") == [
            nested_tx,
            nested_tx,
        ]

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            abort_transaction(nested_tx)

        op.ensure_running()
        new_nested_input_transaction_ids = get(op.get_path() + "/@nested_input_transaction_ids")
        assert len(new_nested_input_transaction_ids) == 2
        assert new_nested_input_transaction_ids[0] == new_nested_input_transaction_ids[1]
        assert new_nested_input_transaction_ids[0] != nested_tx

    @authors("babenko")
    def test_update_lock_transaction_timeout(self):
        lock_tx = get("//sys/scheduler/lock/@locks/0/transaction_id")
        new_timeout = get("#{}/@timeout".format(lock_tx)) + 1234
        set(
            "//sys/scheduler/config/lock_transaction_timeout",
            new_timeout,
            recursive=True,
        )
        wait(lambda: get("#{}/@timeout".format(lock_tx)) == new_timeout)

    @authors("max42")
    def test_controller_throttling(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")
        for i in range(25):
            write_table("<append=%true>//tmp/t_in", [{"a": i}])

        def get_controller_throttling_schedule_job_fail_count():
            op = map(
                in_=["//tmp/t_in"],
                out=["//tmp/t_out"],
                command="cat",
                spec={
                    "job_count": 5,
                    "testing": {
                        "build_job_spec_proto_delay": 1000,
                    },
                },
            )
            schedule_job_statistics = get(op.get_path() + "/@progress/schedule_job_statistics")
            return schedule_job_statistics.get("failed", {}).get("controller_throttling", 0)

        if not exists("//sys/controller_agents/config/operation_options"):
            set("//sys/controller_agents/config/operation_options", {})

        job_spec_count_limit_path = (
            "//sys/controller_agents/config/operation_options/controller_building_job_spec_count_limit"
        )
        total_job_spec_slice_count_limit_path = (
            "//sys/controller_agents/config/operation_options/controller_total_building_job_spec_slice_count_limit"
        )
        controller_agent_config_revision_path = (
            "//sys/controller_agents/instances/{}/orchid/controller_agent/config_revision".format(
                ls("//sys/controller_agents/instances")[0]
            )
        )

        def wait_for_fresh_config():
            config_revision = get(controller_agent_config_revision_path)
            wait(lambda: get(controller_agent_config_revision_path) - config_revision >= 2)

        assert get_controller_throttling_schedule_job_fail_count() == 0

        try:
            set(job_spec_count_limit_path, 1)
            wait_for_fresh_config()
            assert get_controller_throttling_schedule_job_fail_count() > 0
        finally:
            remove(job_spec_count_limit_path, force=True)

        try:
            set(total_job_spec_slice_count_limit_path, 5)
            wait_for_fresh_config()
            assert get_controller_throttling_schedule_job_fail_count() > 0
        finally:
            remove(total_job_spec_slice_count_limit_path, force=True)

        wait_for_fresh_config()
        assert get_controller_throttling_schedule_job_fail_count() == 0

    @authors("alexkolodezny")
    def test_suspention_on_job_failure(self):
        op = run_test_vanilla(
            "exit 1",
            spec={"suspend_on_job_failure": True},
            fail_fast=False
        )
        wait(lambda: get(op.get_path() + "/@suspended"))


class TestSchedulerCommonMulticell(TestSchedulerCommon):
    NUM_TEST_PARTITIONS = 6
    NUM_SECONDARY_MASTER_CELLS = 2


##################################################################


class TestMultipleSchedulers(YTEnvSetup, PrepareTables):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 2

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "connect_retry_backoff_time": 1000,
            "fair_share_update_period": 100,
            "profiling_update_period": 100,
            "testing_options": {
                "master_disconnect_delay": 3000,
            },
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "snapshot_period": 500,
        }
    }

    def _get_scheduler_transation(self):
        while True:
            scheduler_locks = get("//sys/scheduler/lock/@locks", verbose=False)
            if len(scheduler_locks) > 0:
                scheduler_transaction = scheduler_locks[0]["transaction_id"]
                return scheduler_transaction
            time.sleep(0.01)

    @authors("ignat")
    def test_hot_standby(self):
        self._prepare_tables()

        op = map(track=False, in_="//tmp/t_in", out="//tmp/t_out", command="cat; sleep 15")

        op.wait_for_fresh_snapshot()

        transaction_id = self._get_scheduler_transation()

        def get_transaction_title(transaction_id):
            return get("#{0}/@title".format(transaction_id), verbose=False)

        title = get_transaction_title(transaction_id)

        while True:
            abort_transaction(transaction_id)

            new_transaction_id = self._get_scheduler_transation()
            new_title = get_transaction_title(new_transaction_id)
            if title != new_title:
                break

            title = new_title
            transaction_id = new_transaction_id
            time.sleep(0.3)

        op.track()

        assert read_table("//tmp/t_out") == [{"foo": "bar"}]


##################################################################


class TestSchedulerMaxChunkPerJob(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "map_operation_options": {
                "max_data_slices_per_job": 1,
            },
            "ordered_merge_operation_options": {
                "max_data_slices_per_job": 1,
            },
        }
    }

    @authors("ignat")
    def test_max_data_slices_per_job(self):
        data = [{"foo": i} for i in range(5)]
        create("table", "//tmp/in1")
        create("table", "//tmp/in2")
        create("table", "//tmp/out")
        write_table("//tmp/in1", data, sorted_by="foo")
        write_table("//tmp/in2", data, sorted_by="foo")

        op = merge(
            mode="ordered",
            in_=["//tmp/in1", "//tmp/in2"],
            out="//tmp/out",
            spec={"force_transform": True},
        )
        assert data + data == read_table("//tmp/out")

        # Must be 2 jobs since input has 2 chunks.
        assert get(op.get_path() + "/@progress/jobs/total") == 2

        op = map(command="cat >/dev/null", in_=["//tmp/in1", "//tmp/in2"], out="//tmp/out")
        assert get(op.get_path() + "/@progress/jobs/total") == 2

    @authors("babenko")
    def test_lock_revisions_yt_13962(self):
        tx = start_transaction()
        create("table", "//tmp/t", tx=tx)
        data = [{"key": "value"}]
        write_table("//tmp/t", data, tx=tx)
        merge(in_=["//tmp/t"], out="//tmp/t", mode="ordered", tx=tx)
        assert read_table("//tmp/t", tx=tx) == data


##################################################################

class TestSchedulerMaxInputOutputTableCount(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "max_input_table_count": 2,
            "max_output_table_count": 2
        }
    }

    @authors("alexkolodezny")
    def test_max_input_table_count(self):
        create("table", "//tmp/in1")
        create("table", "//tmp/in2")
        create("table", "//tmp/in3")
        create("table", "//tmp/out")

        with raises_yt_error("Too many input tables: maximum allowed 2, actual 3"):
            map(
                command="",
                in_=["//tmp/in1", "//tmp/in2/", "//tmp/in3"],
                out="//tmp/out"
            )

    @authors("alexkolodezny")
    def test_max_output_table_count(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out1")
        create("table", "//tmp/out2")
        create("table", "//tmp/out3")

        with raises_yt_error("Too many output tables: maximum allowed 2, actual 3"):
            map(
                command="",
                in_="//tmp/in",
                out=["//tmp/out1", "//tmp/out2", "//tmp/out3"]
            )


##################################################################

class TestSchedulerMaxChildrenPerAttachRequest(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "max_children_per_attach_request": 1,
        }
    }

    @authors("ignat")
    def test_max_children_per_attach_request(self):
        data = [{"foo": i} for i in range(3)]
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", data)

        map(
            command="cat",
            in_="//tmp/in",
            out="//tmp/out",
            spec={"data_size_per_job": 1},
        )

        assert sorted_dicts(read_table("//tmp/out")) == sorted_dicts(data)
        assert get("//tmp/out/@row_count") == 3

    @authors("ignat")
    def test_max_children_per_attach_request_in_live_preview(self):
        data = [{"foo": i} for i in range(3)]
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", data)

        op = map(
            track=False,
            command=with_breakpoint("cat ; BREAKPOINT"),
            in_="//tmp/in",
            out="//tmp/out",
            spec={"data_size_per_job": 1},
        )

        jobs = wait_breakpoint(job_count=3)

        for job_id in jobs[:2]:
            release_breakpoint(job_id=job_id)

        for _ in range(100):
            jobs_exist = exists(op.get_path() + "/@progress/jobs")
            if jobs_exist:
                completed_jobs = get(op.get_path() + "/@progress/jobs/completed/total")
                if completed_jobs == 2:
                    break
            time.sleep(0.1)

        transaction_id = get(op.get_path() + "/@async_scheduler_transaction_id")
        wait(lambda: get(op.get_path() + "/output_0/@row_count", tx=transaction_id) == 2)

        release_breakpoint()
        op.track()


##################################################################


class TestSchedulerConfig(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "event_log": {"retry_backoff_time": 7, "flush_period": 5000},
        },
        "addresses": [("ipv4", "127.0.0.1"), ("ipv6", "::1")],
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "event_log": {"retry_backoff_time": 7, "flush_period": 5000},
            "operation_options": {"spec_template": {"data_weight_per_job": 1000}},
            "map_operation_options": {
                "spec_template": {
                    "data_weight_per_job": 2000,
                    "max_failed_job_count": 10,
                }
            },
            "environment": {"TEST_VAR": "10"},
        },
        "addresses": [("ipv4", "127.0.0.1"), ("ipv6", "::1")],
    }

    @authors("ignat")
    def test_basic(self):
        orchid_scheduler_config = "//sys/scheduler/orchid/scheduler/config"
        assert get("{0}/event_log/flush_period".format(orchid_scheduler_config)) == 5000
        assert get("{0}/event_log/retry_backoff_time".format(orchid_scheduler_config)) == 7

        set("//sys/scheduler/config", {"event_log": {"flush_period": 10000}})

        wait(lambda: get("{0}/event_log/flush_period".format(orchid_scheduler_config)) == 10000)
        wait(lambda: get("{0}/event_log/retry_backoff_time".format(orchid_scheduler_config)) == 7)

        set("//sys/scheduler/config", {})

        wait(lambda: get("{0}/event_log/flush_period".format(orchid_scheduler_config)) == 5000)
        wait(lambda: get("{0}/event_log/retry_backoff_time".format(orchid_scheduler_config)) == 7)

    @authors("ignat")
    def test_adresses(self):
        adresses = get("//sys/scheduler/@addresses")
        assert adresses["ipv4"].startswith("127.0.0.1:")
        assert adresses["ipv6"].startswith("::1:")

    @authors("ignat")
    def test_specs(self):
        create("table", "//tmp/t_in")
        write_table("<append=true;sorted_by=[foo]>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out")

        op = map(command="sleep 1000", in_=["//tmp/t_in"], out="//tmp/t_out", track=False, fail_fast=False)

        full_spec_path = "//sys/scheduler/orchid/scheduler/operations/{0}/full_spec".format(op.id)
        wait(lambda: exists(full_spec_path))

        assert get("{}/data_weight_per_job".format(full_spec_path)) == 2000
        assert get("{}/max_failed_job_count".format(full_spec_path)) == 10

        op.abort()

        op = reduce(
            command="sleep 1000",
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            reduce_by=["foo"],
            track=False,
            fail_fast=False,
        )
        wait(lambda: op.get_state() == "running")

        full_spec_path = "//sys/scheduler/orchid/scheduler/operations/{0}/full_spec".format(op.id)
        wait(lambda: exists(full_spec_path))

        assert get("{}/data_weight_per_job".format(full_spec_path)) == 1000
        assert get("{}/max_failed_job_count".format(full_spec_path)) == 10

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        op.ensure_running()

        assert get("{}/data_weight_per_job".format(full_spec_path)) == 1000
        assert get("{}/max_failed_job_count".format(full_spec_path)) == 10

        op.abort()

    @authors("ignat")
    def test_unrecognized_spec(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"a": "b"}])
        create("table", "//tmp/t_out")
        op = map(
            command="sleep 1000",
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            track=False,
            spec={"xxx": "yyy"},
        )

        wait(lambda: exists(op.get_path() + "/@unrecognized_spec"))
        assert get(op.get_path() + "/@unrecognized_spec") == {"xxx": "yyy"}

    @authors("ignat")
    def test_brief_progress(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"a": "b"}])
        create("table", "//tmp/t_out")
        op = map(command="sleep 1000", in_=["//tmp/t_in"], out="//tmp/t_out", track=False)

        wait(lambda: exists(op.get_path() + "/@brief_progress"))
        assert "jobs" in list(get(op.get_path() + "/@brief_progress"))

    @authors("ignat")
    def test_cypress_config(self):
        create("table", "//tmp/t_in")
        write_table("<append=true>//tmp/t_in", {"foo": "bar"})
        create("table", "//tmp/t_out")

        op = map(command="cat", in_=["//tmp/t_in"], out="//tmp/t_out", fail_fast=False)
        assert get(op.get_path() + "/@full_spec/data_weight_per_job") == 2000
        assert get(op.get_path() + "/@full_spec/max_failed_job_count") == 10

        set(
            "//sys/controller_agents/config",
            {
                "map_operation_options": {"spec_template": {"max_failed_job_count": 50}},
                "environment": {"OTHER_VAR": "20"},
            },
        )

        instances = ls("//sys/controller_agents/instances")
        for instance in instances:
            config_path = "//sys/controller_agents/instances/{0}/orchid/controller_agent/config".format(instance)
            wait(
                lambda: exists(config_path + "/environment/OTHER_VAR")
                and get(config_path + "/environment/OTHER_VAR") == "20"
            )

            environment = get(config_path + "/environment")
            assert environment["TEST_VAR"] == "10"
            assert environment["OTHER_VAR"] == "20"

            assert get(config_path + "/map_operation_options/spec_template/max_failed_job_count") == 50

        op = map(command="cat", in_=["//tmp/t_in"], out="//tmp/t_out", fail_fast=False)
        assert get(op.get_path() + "/@full_spec/data_weight_per_job") == 2000
        assert get(op.get_path() + "/@full_spec/max_failed_job_count") == 50

    @authors("ignat")
    def test_min_spare_job_resources_on_node(self):
        orchid_scheduler_config = "//sys/scheduler/orchid/scheduler/config"
        min_spare_job_resources = get("{0}/min_spare_job_resources_on_node".format(orchid_scheduler_config))
        assert min_spare_job_resources["cpu"] == 1.0
        assert min_spare_job_resources["user_slots"] == 1
        assert min_spare_job_resources["memory"] == 256 * 1024 * 1024

        set("//sys/scheduler/config/min_spare_job_resources_on_node", {"user_slots": 2})
        wait(lambda: get("{0}/min_spare_job_resources_on_node".format(orchid_scheduler_config)) == {"user_slots": 2})

##################################################################


class TestSchedulerOperationSnapshots(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "max_concurrent_controller_schedule_job_calls": 1,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "snapshot_period": 500,
            "operation_controller_suspend_timeout": 2000,
        }
    }

    @authors("ignat")
    def test_snapshots(self):
        create("table", "//tmp/in")
        write_table("//tmp/in", [{"foo": i} for i in range(5)])
        create("table", "//tmp/out")

        testing_options = {"controller_scheduling_delay": {"duration": 500}}

        op = map(
            track=False,
            command=with_breakpoint("cat ; BREAKPOINT"),
            in_="//tmp/in",
            out="//tmp/out",
            spec={"data_weight_per_job": 1, "testing": testing_options},
        )

        snapshot_path = op.get_path() + "/snapshot"
        wait(lambda: exists(snapshot_path))

        # This is done to avoid read failures due to snapshot file rewriting.
        snapshot_backup_path = snapshot_path + ".backup"
        copy(snapshot_path, snapshot_backup_path)
        assert len(read_file(snapshot_backup_path, verbose=False)) > 0

        ts_str = get(op.get_path() + "/controller_orchid/progress/last_successful_snapshot_time")
        assert time.time() - date_string_to_timestamp(ts_str) < 60

        release_breakpoint()
        op.track()

    @authors("ignat")
    def test_parallel_snapshots(self):
        create("table", "//tmp/input")

        testing_options = {"controller_scheduling_delay": {"duration": 100}}

        job_count = 1
        original_data = [{"index": i} for i in range(job_count)]
        write_table("//tmp/input", original_data)

        operation_count = 5
        ops = []
        for index in range(operation_count):
            output = "//tmp/output" + str(index)
            create("table", output)
            ops.append(
                map(
                    track=False,
                    command=with_breakpoint("cat ; BREAKPOINT"),
                    in_="//tmp/input",
                    out=[output],
                    spec={"data_size_per_job": 1, "testing": testing_options},
                )
            )

        for op in ops:
            snapshot_path = op.get_path() + "/snapshot"
            wait(lambda: exists(snapshot_path))

            snapshot_backup_path = snapshot_path + ".backup"
            copy(snapshot_path, snapshot_backup_path)
            assert len(read_file(snapshot_backup_path, verbose=False)) > 0

        # All our operations use 'default' breakpoint so we release it and all operations continue execution.
        release_breakpoint()

        for op in ops:
            op.track()

    @authors("ignat")
    def test_suspend_time_limit(self):
        create("table", "//tmp/in")
        write_table("//tmp/in", [{"foo": i} for i in range(5)])

        create("table", "//tmp/out1")
        create("table", "//tmp/out2")

        while True:
            op2 = map(
                track=False,
                command="cat",
                in_="//tmp/in",
                out="//tmp/out2",
                spec={
                    "data_size_per_job": 1,
                    "testing": {"delay_inside_suspend": 15000},
                },
            )

            time.sleep(2)
            snapshot_path2 = op2.get_path() + "/snapshot"
            if exists(snapshot_path2):
                op2.abort()
                continue
            else:
                break

        op1 = map(
            track=False,
            command="sleep 10; cat",
            in_="//tmp/in",
            out="//tmp/out1",
            spec={"data_size_per_job": 1},
        )

        snapshot_path1 = op1.get_path() + "/snapshot"
        snapshot_path2 = op2.get_path() + "/snapshot"

        wait(lambda: exists(snapshot_path1))
        assert not exists(snapshot_path2)

        op1.track()
        op2.track()


##################################################################


class TestSchedulerHeterogeneousConfiguration(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    @classmethod
    def modify_node_config(cls, config):
        if not hasattr(cls, "node_counter"):
            cls.node_counter = 0
        cls.node_counter += 1
        if cls.node_counter == 1:
            config["exec_agent"]["job_controller"]["resource_limits"]["user_slots"] = 0

    @authors("renadeen", "ignat")
    def test_job_count(self):
        data = [{"foo": i} for i in range(3)]
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", data)

        wait(lambda: get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/user_slots") == 2)
        wait(lambda: get("//sys/scheduler/orchid/scheduler/cluster/resource_usage/user_slots") == 0)

        wait(
            lambda: get(
                "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/resource_limits/user_slots"
            )
            == 2
        )
        wait(
            lambda: get(
                "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/resource_usage/user_slots"
            )
            == 0
        )

        op = map(
            track=False,
            command="sleep 100",
            in_="//tmp/in",
            out="//tmp/out",
            spec={"data_size_per_job": 1, "locality_timeout": 0},
        )

        wait(
            lambda: op.get_runtime_progress("scheduling_info_per_pool_tree/default/resource_usage/user_slots", 0) == 2
        )
        wait(lambda: get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/user_slots") == 2)
        wait(lambda: get("//sys/scheduler/orchid/scheduler/cluster/resource_usage/user_slots") == 2)

        wait(
            lambda: get(
                "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/resource_limits/user_slots"
            )
            == 2
        )
        wait(
            lambda: get(
                "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/resource_usage/user_slots"
            )
            == 2
        )


###############################################################################################


class TestSchedulerJobStatistics(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {"heartbeat_period": 100},  # 100 msec
            "controller_agent_connector": {"heartbeat_period": 100},  # 100 msec
        },
    }

    def _create_table(self, table):
        create("table", table)
        set(table + "/@replication_factor", 1)

    @authors("ignat")
    def test_scheduler_job_by_id(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")
        write_table("//tmp/in", [{"foo": i} for i in range(10)])

        op = map(
            track=False,
            label="scheduler_job_statistics",
            in_="//tmp/in",
            out="//tmp/out",
            command=with_breakpoint("BREAKPOINT ; cat"),
        )

        wait_breakpoint()
        running_jobs = op.get_running_jobs()
        job_id = next(iter(running_jobs.keys()))
        job_info = next(iter(running_jobs.values()))

        # Check that /jobs is accessible only with direct job id.
        with pytest.raises(YtError):
            get("//sys/scheduler/orchid/scheduler/jobs")
        with pytest.raises(YtError):
            ls("//sys/scheduler/orchid/scheduler/jobs")

        job_info2 = get("//sys/scheduler/orchid/scheduler/jobs/{0}".format(job_id))
        # Check that job_info2 contains all the keys that are in job_info (do not check the same
        # for values because values could actually change between two get requests).
        for key in job_info:
            assert key in job_info2

        with pytest.raises(YtError):
            get("//sys/scheduler/orchid/scheduler/jobs/1-2-3-4")

    @authors("ignat")
    def test_scheduler_job_statistics(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")
        write_table("//tmp/in", [{"foo": i} for i in range(10)])

        op = map(
            track=False,
            label="scheduler_job_statistics",
            in_="//tmp/in",
            out="//tmp/out",
            command=with_breakpoint("cat ; BREAKPOINT"),
        )

        wait_breakpoint()
        running_jobs = op.get_running_jobs()
        job_id = next(iter(running_jobs.keys()))

        statistics_appeared = False
        for _ in range(300):
            statistics = get("//sys/scheduler/orchid/scheduler/jobs/{0}/statistics".format(job_id))
            data = statistics.get("data", {})
            _input = data.get("input", {})
            row_count = _input.get("row_count", {})
            _sum = row_count.get("sum", 0)
            if _sum == 10:
                statistics_appeared = True
                break
            time.sleep(0.1)

        assert statistics_appeared

        traffic_statistics = statistics["job_proxy"]["traffic"]
        assert traffic_statistics["inbound"]["from_"]["sum"] > 0
        assert traffic_statistics["duration_ms"]["sum"] > 0
        assert traffic_statistics["_to_"]["sum"] > 0

        release_breakpoint()
        op.track()

    @authors("ignat")
    def test_scheduler_operation_statistics(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")
        write_table("//tmp/in", [{"foo": i} for i in range(10)])

        op = map(
            in_="//tmp/in",
            out="//tmp/out",
            command="cat",
            spec={"data_size_per_job": 1})

        statistics = get(op.get_path() + "/@progress/job_statistics_v2")
        assert extract_statistic_v2(statistics, "time.exec", summary_type="count") == 10
        assert extract_statistic_v2(statistics, "time.exec") <= \
            extract_statistic_v2(statistics, "time.total")

    @authors("ignat")
    def test_statistics_for_aborted_operation(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")
        write_table("//tmp/in", [{"foo": i} for i in range(3)])

        op = map(
            track=False,
            in_="//tmp/in",
            out="//tmp/out",
            command=with_breakpoint("cat ; BREAKPOINT"),
            spec={"data_size_per_job": 1})

        wait_breakpoint()

        running_jobs = op.get_running_jobs()

        for job_id in running_jobs:
            statistics_appeared = False
            for _ in range(300):
                statistics = get("//sys/scheduler/orchid/scheduler/jobs/{0}/statistics".format(job_id))
                data = statistics.get("data", {})
                _input = data.get("input", {})
                row_count = _input.get("row_count", {})
                _sum = row_count.get("sum", 0)
                if _sum > 0:
                    statistics_appeared = True
                    break
                time.sleep(0.1)
            assert statistics_appeared

        op.abort()

        assert_statistics(
            op,
            key="time.total",
            assertion=lambda count: count == 3,
            job_state="aborted",
            job_type="map",
            summary_type="count")


##################################################################

class TestNewLivePreview(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 3

    @authors("max42", "gritukan")
    def test_new_live_preview_simple(self):
        data = [{"foo": i} for i in range(3)]

        create("table", "//tmp/t1")
        write_table("//tmp/t1", data)

        create("table", "//tmp/t2")

        op = map(
            wait_for_jobs=True,
            track=False,
            command=with_breakpoint("BREAKPOINT ; cat"),
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"data_size_per_job": 1},
        )

        jobs = wait_breakpoint(job_count=3)

        assert exists(op.get_path() + "/controller_orchid")

        release_breakpoint(job_id=jobs[0])
        release_breakpoint(job_id=jobs[1])
        wait(lambda: op.get_job_count("completed") == 2)

        live_preview_path = op.get_path() + "/controller_orchid/data_flow_graph/vertices/map/live_previews/0"
        live_preview_data = read_table(live_preview_path)
        assert len(live_preview_data) == 2
        assert all(record in data for record in live_preview_data)

        create("table", "//tmp/lp")
        concatenate([live_preview_path], "//tmp/lp")

        release_breakpoint(job_id=jobs[2])
        op.track()

        live_preview_data = read_table("//tmp/lp")
        assert len(live_preview_data) == 2
        assert all(record in data for record in live_preview_data)

    @authors("max42", "gritukan")
    def test_new_live_preview_intermediate_data_acl(self):
        create_user("u1")
        create_user("u2")

        data = [{"foo": i} for i in range(3)]

        create("table", "//tmp/t1")
        write_table("//tmp/t1", data)

        create("table", "//tmp/t2")

        op = map(
            wait_for_jobs=True,
            track=False,
            command=with_breakpoint("BREAKPOINT ; cat"),
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={
                "data_size_per_job": 1,
                "acl": [make_ace("allow", "u1", "read")],
            },
        )

        jobs = wait_breakpoint(job_count=2)

        assert exists(op.get_path() + "/controller_orchid")

        release_breakpoint(job_id=jobs[0])
        release_breakpoint(job_id=jobs[1])
        wait(lambda: op.get_job_count("completed") == 2)

        read_table(
            op.get_path() + "/controller_orchid/data_flow_graph/vertices/map/live_previews/0",
            authenticated_user="u1",
        )

        with pytest.raises(YtError):
            read_table(
                op.get_path() + "/controller_orchid/data_flow_graph/vertices/map/live_previews/0",
                authenticated_user="u2",
            )

    @authors("max42", "gritukan")
    def test_new_live_preview_ranges(self):
        create("table", "//tmp/t1")
        for i in range(3):
            write_table("<append=%true>//tmp/t1", [{"a": i}])

        create("table", "//tmp/t2")

        op = map_reduce(
            wait_for_jobs=True,
            track=False,
            mapper_command='for ((i=0; i<3; i++)); do echo "{a=$(($YT_JOB_INDEX*3+$i))};"; done',
            reducer_command=with_breakpoint("cat; BREAKPOINT"),
            reduce_by="a",
            sort_by=["a"],
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"map_job_count": 3, "partition_count": 1},
        )

        wait(lambda: op.get_job_count("completed") == 3)

        assert exists(op.get_path() + "/controller_orchid")

        live_preview_path = (
            op.get_path() + "/controller_orchid/data_flow_graph/vertices/partition_map(0)/live_previews/0"
        )
        live_preview_data = read_table(live_preview_path)

        assert len(live_preview_data) == 9

        # We try all possible combinations of chunk and row index ranges and check that everything works as expected.
        expected_all_ranges_data = []
        all_ranges = []
        for lower_row_index in list(range(10)) + [None]:
            for upper_row_index in list(range(10)) + [None]:
                for lower_chunk_index in list(range(4)) + [None]:
                    for upper_chunk_index in list(range(4)) + [None]:
                        lower_limit = dict()
                        real_lower_index = 0
                        if lower_row_index is not None:
                            lower_limit["row_index"] = lower_row_index
                            real_lower_index = max(real_lower_index, lower_row_index)
                        if lower_chunk_index is not None:
                            lower_limit["chunk_index"] = lower_chunk_index
                            real_lower_index = max(real_lower_index, lower_chunk_index * 3)

                        upper_limit = dict()
                        real_upper_index = 9
                        if upper_row_index is not None:
                            upper_limit["row_index"] = upper_row_index
                            real_upper_index = min(real_upper_index, upper_row_index)
                        if upper_chunk_index is not None:
                            upper_limit["chunk_index"] = upper_chunk_index
                            real_upper_index = min(real_upper_index, upper_chunk_index * 3)

                        all_ranges.append({"lower_limit": lower_limit, "upper_limit": upper_limit})
                        expected_all_ranges_data += [live_preview_data[real_lower_index:real_upper_index]]

        all_ranges_path = (
            b"<"
            + yson.dumps({"ranges": all_ranges}, yson_type="map_fragment", yson_format="text")
            + b">"
            + live_preview_path.encode("ascii")
        )

        all_ranges_data = read_table(all_ranges_path, verbose=False)

        position = 0
        for i, range_ in enumerate(expected_all_ranges_data):
            if all_ranges_data[position:position + len(range_)] != range_:
                print_debug("position =", position, ", range =", all_ranges[i])
                print_debug("expected:", range_)
                print_debug("actual:", all_ranges_data[position:position + len(range_)])
                assert all_ranges_data[position:position + len(range_)] == range_
            position += len(range_)

        release_breakpoint()
        op.track()

    @authors("max42", "gritukan")
    def test_disabled_live_preview(self):
        create_user("robot-root")
        add_member("robot-root", "superusers")

        data = [{"foo": i} for i in range(3)]

        create("table", "//tmp/t1")
        write_table("//tmp/t1", data)

        create("table", "//tmp/t2")

        # Run operation with given params and return a tuple (live preview created, suppression alert set)
        def check_live_preview(enable_legacy_live_preview=None, authenticated_user=None, index=None):
            op = map(
                wait_for_jobs=True,
                track=False,
                command=with_breakpoint("BREAKPOINT ; cat", breakpoint_name=str(index)),
                in_="//tmp/t1",
                out="//tmp/t2",
                spec={
                    "data_size_per_job": 1,
                    "enable_legacy_live_preview": enable_legacy_live_preview,
                },
                authenticated_user=authenticated_user,
            )

            wait_breakpoint(job_count=2, breakpoint_name=str(index))

            async_transaction_id = get(op.get_path() + "/@async_scheduler_transaction_id")
            live_preview_created = exists(op.get_path() + "/output_0", tx=async_transaction_id)
            suppression_alert_set = "legacy_live_preview_suppressed" in op.get_alerts()

            op.abort()

            return (live_preview_created, suppression_alert_set)

        combinations = [
            (None, "root", True, False),
            (True, "root", True, False),
            (False, "root", False, False),
            (None, "robot-root", False, True),
            (True, "robot-root", True, False),
            (False, "robot-root", False, False),
        ]

        for i, combination in enumerate(combinations):
            (
                enable_legacy_live_preview,
                authenticated_user,
                live_preview_created,
                suppression_alert_set,
            ) = combination
            assert (
                check_live_preview(
                    enable_legacy_live_preview=enable_legacy_live_preview,
                    authenticated_user=authenticated_user,
                    index=i,
                )
                == (live_preview_created, suppression_alert_set)
            )


class TestNewLivePreviewMulticell(TestNewLivePreview):
    NUM_SECONDARY_MASTER_CELLS = 2


##################################################################


class TestConnectToMaster(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_SCHEDULERS = 1
    NUM_NODES = 0

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "connect_retry_backoff_time": 1000
        }
    }

    @authors("max42")
    def test_scheduler_doesnt_connect_to_master_in_safe_mode(self):
        set("//sys/@config/enable_safe_mode", True)
        self.Env.kill_schedulers()
        self.Env.start_schedulers(sync=False)
        time.sleep(1)

        wait(lambda: self.has_safe_mode_error_in_log())

    def has_safe_mode_error_in_log(self):
        with open(self.path_to_run + "/logs/scheduler-0.log.zst", "rb") as file:
            decompressor = zstd.ZstdDecompressor()
            binary_reader = decompressor.stream_reader(file, read_size=8192)
            text_stream = io.TextIOWrapper(binary_reader, encoding='utf-8')
            for line in text_stream:
                if "Error connecting to master" in line and "Cluster is in safe mode" in line:
                    return True
        return False

    @authors("renadeen")
    def test_scheduler_doesnt_start_with_invalid_pools(self):
        alerts = get("//sys/scheduler/@alerts")
        assert [element for element in alerts if element["attributes"]["alert_type"] == "scheduler_cannot_connect"] == []
        with Restarter(self.Env, SCHEDULERS_SERVICE, sync=False):
            move("//sys/pool_trees", "//sys/pool_trees_bak")
            set("//sys/pool_trees", {"default": {"invalid_pool": 1}})
            set("//sys/pool_trees/default/@config", {})

        wait(
            lambda: [
                element for element in get("//sys/scheduler/@alerts")
                if element["attributes"]["alert_type"] == "scheduler_cannot_connect"
            ] != []
        )
        alerts = [
            element for element in get("//sys/scheduler/@alerts")
            if element["attributes"]["alert_type"] == "scheduler_cannot_connect"
        ]
        assert len(alerts) == 1
        assert alerts[0]["attributes"]["alert_type"] == "scheduler_cannot_connect"

        scheduler = ls("//sys/scheduler/instances")[0]
        assert not get("//sys/scheduler/instances/" + scheduler + "/orchid/scheduler/service/connected")

        remove("//sys/pool_trees")
        move("//sys/pool_trees_bak", "//sys/pool_trees")
        wait(lambda: get("//sys/scheduler/instances/" + scheduler + "/orchid/scheduler/service/connected"))


##################################################################


class TestEventLog(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1
    NUM_CONTROLLER_AGENTS = 1
    USE_PORTO = True

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "event_log": {
                "flush_period": 1000,
            },
            "accumulated_usage_log_period": 1000,
            "accumulated_resource_usage_update_period": 100,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {"controller_agent": {"event_log": {"flush_period": 1000}}}

    @authors("ignat")
    def test_scheduler_event_log(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": "b"}])
        op = map(
            in_="//tmp/t1",
            out="//tmp/t2",
            command='cat; bash -c "for (( I=0 ; I<=100*1000 ; I++ )) ; do echo $(( I+I*I )); done; sleep 2" >/dev/null',
        )

        def check_statistics(statistics, statistic_extractor):
            statistic_extractor(statistics, "user_job.cpu.user") > 0
            statistic_extractor(statistics, "user_job.block_io.bytes_read") is not None
            statistic_extractor(statistics, "user_job.current_memory.rss") > 0
            statistic_extractor(statistics, "user_job.max_memory") > 0
            statistic_extractor(statistics, "user_job.cumulative_memory_mb_sec") > 0
            statistic_extractor(statistics, "job_proxy.cpu.user") == 1
            statistic_extractor(statistics, "job_proxy.cpu.user") == 1

        statistics_v2 = get(op.get_path() + "/@progress/job_statistics_v2")
        check_statistics(statistics_v2, extract_statistic_v2)

        deprecated_statistics = get(op.get_path() + "/@progress/job_statistics")
        check_statistics(deprecated_statistics, extract_deprecated_statistic)

        # wait for scheduler to dump the event log
        def check():
            def get_statistics(statistics, complex_key):
                result = statistics
                for part in complex_key.split("."):
                    if part:
                        if part not in result:
                            return None
                        result = result[part]
                return result

            res = read_table("//sys/scheduler/event_log")
            event_types = builtins.set()
            for item in res:
                event_types.add(item["event_type"])
                if item["event_type"] == "job_completed":
                    stats = item["statistics"]
                    user_time = get_statistics(stats, "user_job.cpu.user")
                    # our job should burn enough cpu
                    if user_time == 0:
                        return False
                if item["event_type"] == "job_started":
                    limits = item["resource_limits"]
                    if limits["cpu"] == 0:
                        return False
                    if limits["user_memory"] == 0:
                        return False
                    if limits["user_slots"] == 0:
                        return False
            if "operation_started" not in event_types:
                return False
            return True

        wait(check)

    @authors("ignat")
    def test_scheduler_event_log_buffering(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": "b"}])

        for node in ls("//sys/cluster_nodes"):
            set("//sys/cluster_nodes/{0}/@banned".format(node), True)

        time.sleep(2)
        op = map(track=False, in_="//tmp/t1", out="//tmp/t2", command="cat")
        time.sleep(2)

        for node in ls("//sys/cluster_nodes"):
            set("//sys/cluster_nodes/{0}/@banned".format(node), False)

        op.track()

        def check():
            try:
                res = read_table("//sys/scheduler/event_log")
            except YtError:
                return False
            event_types = builtins.set([item["event_type"] for item in res])
            for event in [
                "scheduler_started",
                "operation_started",
                "operation_completed",
            ]:
                if event not in event_types:
                    return False
            return True

        wait(check)

    @authors("ignat")
    def test_structured_event_log(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": "b"}])

        op = map(in_="//tmp/t1", out="//tmp/t2", command="cat")

        # Let's wait until scheduler dumps the information on our map operation
        def check_event_log():
            event_log = read_table("//sys/scheduler/event_log")
            for event in event_log:
                if event["event_type"] == "operation_completed" and event["operation_id"] == op.id:
                    return True
            return False

        wait(check_event_log)

        event_log = read_table("//sys/scheduler/event_log")

        scheduler_log_file = self.path_to_run + "/logs/scheduler-0.json.log"
        scheduler_address = ls("//sys/scheduler/instances")[0]
        scheduler_barrier = write_log_barrier(scheduler_address)

        controller_agent_log_file = self.path_to_run + "/logs/controller-agent-0.json.log"
        controller_agent_address = ls("//sys/controller_agents/instances")[0]
        controller_agent_barrier = write_log_barrier(controller_agent_address)

        structured_log = read_structured_log(scheduler_log_file, to_barrier=scheduler_barrier,
                                             row_filter=lambda e: "event_type" in e)
        structured_log += read_structured_log(controller_agent_log_file, to_barrier=controller_agent_barrier,
                                              row_filter=lambda e: "event_type" in e)

        for normal_event in event_log:
            flag = False
            for structured_event in structured_log:

                def key(event):
                    return (
                        event["timestamp"],
                        event["event_type"],
                        event["operation_id"] if "operation_id" in event else "",
                    )

                if key(normal_event) == key(structured_event):
                    flag = True
                    break
            assert flag

    @authors("eshcherbin")
    def test_split_fair_share_info_events(self):
        def check_keys(event, included_keys=None, excluded_keys=None):
            if included_keys is not None:
                for key in included_keys:
                    assert key in event
            if excluded_keys is not None:
                for key in excluded_keys:
                    assert key not in event

        def read_fair_share_info_events():
            event_log = read_table("//sys/scheduler/event_log", verbose=False)
            events_by_timestamp = defaultdict(list)
            events_by_snapshot_id = defaultdict(list)
            for event in event_log:
                if event["event_type"] == "fair_share_info":
                    check_keys(event, included_keys=["tree_id", "tree_snapshot_id"])
                    events_by_timestamp[event["timestamp"]].append(event)
                    events_by_snapshot_id[event["tree_snapshot_id"]].append(event)

            return events_by_timestamp, events_by_snapshot_id

        def read_latest_fair_share_info():
            events_by_timestamp, events_by_snapshot_id = read_fair_share_info_events()
            if not events_by_timestamp:
                return None

            for _, events in events_by_timestamp.items():
                assert len(frozenset(e["tree_snapshot_id"] for e in events)) == 1
            for _, events in events_by_snapshot_id.items():
                assert len(frozenset(e["timestamp"] for e in events)) == 1

            return events_by_timestamp[max(events_by_timestamp)]

        def check_events(expected_operation_batch_sizes):
            events = read_latest_fair_share_info()
            if events is None:
                return False

            base_event_keys = ["pools", "pool_count", "resource_distribution_info"]
            operations_info_event_keys = ["operations", "operations_batch_index"]

            base_event_count = 0
            actual_operation_batch_sizes = {}
            for event in events:
                if "pools" in event:
                    check_keys(event, included_keys=base_event_keys, excluded_keys=operations_info_event_keys)
                    base_event_count += 1
                else:
                    check_keys(event, included_keys=operations_info_event_keys, excluded_keys=base_event_keys)
                    actual_operation_batch_sizes[event["operations_batch_index"]] = len(event["operations"])

            assert base_event_count == 1
            assert sorted(actual_operation_batch_sizes) == list(range(len(actual_operation_batch_sizes)))
            actual_operation_batch_sizes = [actual_operation_batch_sizes[batch_index]
                                            for batch_index in range(len(actual_operation_batch_sizes))]
            return expected_operation_batch_sizes == actual_operation_batch_sizes

        wait(lambda: check_events([]))

        set("//sys/pool_trees/default/@config/max_event_log_operation_batch_size", 4)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/max_event_log_operation_batch_size") == 4)
        for i in range(4):
            run_sleeping_vanilla()

        wait(lambda: check_events([4]))

        set("//sys/pool_trees/default/@config/max_event_log_operation_batch_size", 3)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/max_event_log_operation_batch_size") == 3)

        wait(lambda: check_events([3, 1]))

    @authors("ignat")
    def test_accumulated_usage(self):
        create_pool("parent_pool", pool_tree="default")
        create_pool("test_pool", pool_tree="default", parent_name="parent_pool")

        scheduler_address = ls("//sys/scheduler/instances")[0]
        from_barrier = write_log_barrier(scheduler_address)

        op = run_test_vanilla("sleep 5.2", pool="test_pool", track=True)

        scheduler_log_file = self.path_to_run + "/logs/scheduler-0.json.log"
        to_barrier = write_log_barrier(scheduler_address)

        structured_log = read_structured_log(scheduler_log_file, from_barrier=from_barrier, to_barrier=to_barrier,
                                             row_filter=lambda e: "event_type" in e)

        found_accumulated_usage_event_with_op = False
        accumulated_usage = 0.0
        for event in structured_log:
            if event["event_type"] == "accumulated_usage_info":
                assert event["tree_id"] == "default"
                assert "pools" in event
                assert "test_pool" in event["pools"]
                assert "parent_pool" in event["pools"]
                assert event["pools"]["test_pool"]["parent"] == "parent_pool"
                assert event["pools"]["parent_pool"]["parent"] == "<Root>"

                assert "operations" in event
                if op.id in event["operations"]:
                    found_accumulated_usage_event_with_op = True
                    assert event["operations"][op.id]["pool"] == "test_pool"
                    assert event["operations"][op.id]["user"] == "root"
                    assert event["operations"][op.id]["operation_type"] == "vanilla"
                    accumulated_usage += event["operations"][op.id]["accumulated_resource_usage"]["cpu"]

            if event["event_type"] == "operation_completed":
                assert event["operation_id"] == op.id
                assert event["scheduling_info_per_tree"]["default"]["pool"] == "test_pool"
                assert event["scheduling_info_per_tree"]["default"]["ancestor_pools"] == ["parent_pool", "test_pool"]
                accumulated_usage += event["accumulated_resource_usage_per_tree"]["default"]["cpu"]

        assert accumulated_usage >= 5.0

        assert found_accumulated_usage_event_with_op

    @authors("ignat")
    def test_trimmed_annotations(self):
        scheduler_address = ls("//sys/scheduler/instances")[0]
        from_barrier = write_log_barrier(scheduler_address)

        op = run_test_vanilla(
            "sleep 1",
            pool="test_pool",
            spec={
                "annotations": {
                    "tag": "my_value",
                    "long_key": "x" * 200,
                    "nested_tag": {"key": "value"},
                }
            },
            track=True)

        scheduler_log_file = self.path_to_run + "/logs/scheduler-0.json.log"
        to_barrier = write_log_barrier(scheduler_address)

        structured_log = read_structured_log(scheduler_log_file, from_barrier=from_barrier, to_barrier=to_barrier,
                                             row_filter=lambda e: "event_type" in e)

        for event in structured_log:
            if event["event_type"] == "operation_completed":
                assert event["operation_id"] == op.id
                assert event["trimmed_annotations"]["tag"] == "my_value"
                assert len(event["trimmed_annotations"]["long_key"]) < 100
                assert "nested_tag" not in event["trimmed_annotations"]

##################################################################


class TestJobStatisticsPorto(YTEnvSetup):
    NUM_SCHEDULERS = 1
    USE_PORTO = True

    @authors("babenko")
    def test_statistics(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": "b"}])
        op = map(
            in_="//tmp/t1",
            out="//tmp/t2",
            command='cat; bash -c "for (( I=0 ; I<=100*1000 ; I++ )) ; do echo $(( I+I*I )); done; sleep 2" >/dev/null',
        )

        def check_statistics(statistics, statistic_extractor):
            for component in ["user_job", "job_proxy"]:
                print_debug(component)
                assert statistic_extractor(statistics, component + ".cpu.user") > 0
                assert statistic_extractor(statistics, component + ".cpu.system") > 0
                assert statistic_extractor(statistics, component + ".cpu.context_switches") is not None
                assert statistic_extractor(statistics, component + ".cpu.peak_thread_count", summary_type="max") is not None
                assert statistic_extractor(statistics, component + ".cpu.wait") is not None
                assert statistic_extractor(statistics, component + ".cpu.throttled") is not None
                assert statistic_extractor(statistics, component + ".block_io.bytes_read") is not None
                assert statistic_extractor(statistics, component + ".max_memory") > 0
            assert statistic_extractor(statistics, "user_job.cumulative_memory_mb_sec") > 0

        statistics_v2 = get(op.get_path() + "/@progress/job_statistics_v2")
        check_statistics(statistics_v2, extract_statistic_v2)

        deprecated_statistics = get(op.get_path() + "/@progress/job_statistics")
        check_statistics(deprecated_statistics, extract_deprecated_statistic)


##################################################################


class TestResourceMetering(YTEnvSetup):
    NUM_SCHEDULERS = 1
    NUM_NODES = 5
    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "resource_metering": {
                "default_abc_id": 42,
            },
            "resource_metering_period": 1000,
        }
    }
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {"user_slots": 3, "cpu": 3}
            }
        }
    }

    @classmethod
    def setup_class(cls):
        super(TestResourceMetering, cls).setup_class()
        set("//sys/@cluster_name", "my_cluster")

    def _extract_metering_records_from_log(self, last=True, schema=None):
        """ Returns dict from metering key to last record with this key. """
        scheduler_log_file = os.path.join(self.path_to_run, "logs/scheduler-0.json.log")
        scheduler_address = ls("//sys/scheduler/instances")[0]
        scheduler_barrier = write_log_barrier(scheduler_address)

        events = read_structured_log(scheduler_log_file, to_barrier=scheduler_barrier,
                                     row_filter=lambda e: "event_type" not in e)

        reports = {}
        for entry in events:
            if "abc_id" not in entry:
                continue

            if schema is not None and entry["schema"] != schema:
                continue

            key = (
                entry["abc_id"],
                entry["labels"]["pool_tree"],
                entry["labels"]["pool"],
            )
            if last:
                reports[key] = (entry["tags"], entry["usage"])
            else:
                if key not in reports:
                    reports[key] = []
                reports[key].append((entry["tags"], entry["usage"]))

        return reports

    def _validate_metering_records(self, root_key, desired_metering_data, event_key_to_last_record, precision=None):
        if root_key not in event_key_to_last_record:
            print_debug("Root key is missing")
            return False
        for key, desired_data in desired_metering_data.items():
            for resource_key, desired_value in desired_data.items():
                tags, usage = event_key_to_last_record.get(key, ({}, {}))
                observed_value = get_by_composite_key(tags, resource_key.split("/"), default=0)
                if isinstance(desired_value, int):
                    observed_value = int(observed_value)
                is_equal = False
                if precision is None:
                    is_equal = observed_value == desired_value
                else:
                    is_equal = abs(observed_value - desired_value) < precision
                if not is_equal:
                    print_debug(
                        "Value mismatch (abc_key: {}, resource_key: {}, observed: {}, desired: {})"
                        .format(key, resource_key, observed_value, desired_value))
                    return False
        return True

    @authors("ignat")
    def test_resource_metering_log(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": "b"}])

        create_pool_tree("yggdrasil", wait_for_orchid=False)
        set("//sys/pool_trees/default/@config/nodes_filter", "!other")
        set("//sys/pool_trees/yggdrasil/@config/nodes_filter", "other")

        nodes = ls("//sys/cluster_nodes")
        for node in nodes[:4]:
            set("//sys/cluster_nodes/" + node + "/@user_tags/end", "other")

        create_pool(
            "abcless",
            pool_tree="yggdrasil",
            attributes={
                "strong_guarantee_resources": {"cpu": 4},
            },
            wait_for_orchid=False,
        )

        create_pool(
            "pixies",
            pool_tree="yggdrasil",
            attributes={
                "strong_guarantee_resources": {"cpu": 3},
                "abc": {"id": 1, "slug": "pixies", "name": "Pixies"},
            },
            wait_for_orchid=False,
        )

        create_pool(
            "francis",
            pool_tree="yggdrasil",
            attributes={
                "strong_guarantee_resources": {"cpu": 1},
                "abc": {"id": 2, "slug": "francis", "name": "Francis"},
            },
            parent_name="pixies",
            wait_for_orchid=False,
        )

        create_pool(
            "misirlou",
            pool_tree="yggdrasil",
            parent_name="pixies",
            # Intentionally wait for pool creation.
            wait_for_orchid=True,
        )

        create_pool(
            "nidhogg",
            pool_tree="yggdrasil",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 5},
                    "burst_guarantee_resources": {"cpu": 6},
                },
                "abc": {"id": 3, "slug": "nidhogg", "name": "Nidhogg"},
            },
            # Intentionally wait for pool creation.
            wait_for_orchid=True,
        )

        op1 = run_test_vanilla("sleep 1000", job_count=2, spec={"pool": "francis", "pool_trees": ["yggdrasil"]})
        op2 = run_test_vanilla("sleep 1000", job_count=1, spec={"pool": "nidhogg", "pool_trees": ["yggdrasil"]})
        op3 = run_test_vanilla("sleep 1000", job_count=1, spec={"pool": "misirlou", "pool_trees": ["yggdrasil"]})
        op4 = run_test_vanilla("sleep 1000", job_count=1, spec={"pool": "abcless", "pool_trees": ["yggdrasil"]})

        wait(lambda: op1.get_job_count("running") == 2)
        wait(lambda: op2.get_job_count("running") == 1)
        wait(lambda: op3.get_job_count("running") == 1)
        wait(lambda: op4.get_job_count("running") == 1)

        root_key = (42, "yggdrasil", "<Root>")

        desired_metering_data = {
            root_key: {
                "strong_guarantee_resources/cpu": 4,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                # Aggregated from op4 in abcless pool.
                "allocated_resources/cpu": 1,
            },
            (1, "yggdrasil", "pixies"): {
                "strong_guarantee_resources/cpu": 2,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                # Aggregated from op3 in misilrou pool.
                "allocated_resources/cpu": 1},
            (2, "yggdrasil", "francis"): {
                "strong_guarantee_resources/cpu": 1,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 2,
            },
            (3, "yggdrasil", "nidhogg"): {
                "strong_guarantee_resources/cpu": 0,
                "resource_flow/cpu": 5,
                "burst_guarantee_resources/cpu": 6,
                "allocated_resources/cpu": 1,
            },
        }

        def check_structured():
            event_key_to_last_record = self._extract_metering_records_from_log()
            return self._validate_metering_records(root_key, desired_metering_data, event_key_to_last_record)

        wait(check_structured)

    @authors("ignat")
    def test_metering_tags(self):
        set("//sys/pool_trees/default/@config/metering_tags", {"my_tag": "my_value"})
        wait(lambda: get("//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/config/metering_tags"))

        # Check that metering tag cannot be specified without abc attribute.
        with pytest.raises(YtError):
            create_pool(
                "my_pool",
                pool_tree="default",
                attributes={
                    "metering_tags": {"pool_tag": "pool_value"},
                },
                wait_for_orchid=False,
            )

        create_pool(
            "my_pool",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 4},
                "abc": {"id": 1, "slug": "my", "name": "MyService"},
                "metering_tags": {"pool_tag": "pool_value"},
            },
            wait_for_orchid=False,
        )

        root_key = (42, "default", "<Root>")

        desired_metering_data = {
            root_key: {
                "strong_guarantee_resources/cpu": 0,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
                "my_tag": "my_value",
            },
            (1, "default", "my_pool"): {
                "strong_guarantee_resources/cpu": 4,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
                "my_tag": "my_value",
                "pool_tag": "pool_value",
            },
        }

        def check_structured():
            event_key_to_last_record = self._extract_metering_records_from_log()
            return self._validate_metering_records(root_key, desired_metering_data, event_key_to_last_record)

        wait(check_structured)

    @authors("ignat")
    def test_resource_metering_at_root(self):
        create_pool(
            "pool_with_abc",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 4},
                "abc": {"id": 1, "slug": "my1", "name": "MyService1"},
            },
            wait_for_orchid=False,
        )

        create_pool(
            "pool_without_abc",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 2},
            },
            wait_for_orchid=False,
        )

        create_pool(
            "pool_with_abc_at_children",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 2},
                "integral_guarantees": {
                    "guarantee_type": "none",
                    "resource_flow": {"cpu": 5},
                },
            },
            wait_for_orchid=False,
        )

        create_pool(
            "strong_guarantees_pool",
            parent_name="pool_with_abc_at_children",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 1},
                "abc": {"id": 2, "slug": "my2", "name": "MyService2"},
            },
            wait_for_orchid=False,
        )

        create_pool(
            "integral_guarantees_pool",
            parent_name="pool_with_abc_at_children",
            pool_tree="default",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 2},
                },
                "abc": {"id": 3, "slug": "my3", "name": "MyService3"},
            },
            wait_for_orchid=False,
        )

        root_key = (42, "default", "<Root>")

        desired_metering_data = {
            root_key: {
                "strong_guarantee_resources/cpu": 3,  # 2 from pool_without_abc, and 1 from pool_with_abc_at_children
                "resource_flow/cpu": 0,  # none integral resources are not summed to <Root>
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
            },
            (1, "default", "pool_with_abc"): {
                "strong_guarantee_resources/cpu": 4,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
            },
            (2, "default", "strong_guarantees_pool"): {
                "strong_guarantee_resources/cpu": 1,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
            },
            (3, "default", "integral_guarantees_pool"): {
                "strong_guarantee_resources/cpu": 0,
                "resource_flow/cpu": 2,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
            },
        }

        def check_structured():
            event_key_to_last_record = self._extract_metering_records_from_log()
            return self._validate_metering_records(root_key, desired_metering_data, event_key_to_last_record)

        wait(check_structured)

    @authors("ignat")
    def test_metering_with_revive(self):
        set("//sys/pool_trees/default/@config/metering_tags", {"my_tag": "my_value"})
        wait(lambda: get("//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/config/metering_tags"))

        create_pool(
            "my_pool",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 4},
                "abc": {"id": 1, "slug": "my", "name": "MyService"},
                "metering_tags": {"pool_tag": "pool_value"},
            },
            wait_for_orchid=False,
        )

        root_key = (42, "default", "<Root>")

        desired_metering_data = {
            root_key: {
                "strong_guarantee_resources/cpu": 0,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
                "my_tag": "my_value",
            },
            (1, "default", "my_pool"): {
                "strong_guarantee_resources/cpu": 4,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
                "allocated_resources/cpu": 0,
                "my_tag": "my_value",
                "pool_tag": "pool_value",
            },
        }

        def check_expected_tags():
            event_key_to_last_record = self._extract_metering_records_from_log()
            return self._validate_metering_records(root_key, desired_metering_data, event_key_to_last_record)

        wait(check_expected_tags)

        wait(lambda: exists("//sys/scheduler/@last_metering_log_time"))

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            time.sleep(8)

        def check_expected_usage():
            event_key_to_records = self._extract_metering_records_from_log(last=False)
            has_long_record = False
            for tags, usage in event_key_to_records[root_key]:
                print_debug(tags, usage)
                # Record could be split by hour bound, therefore we check half of the slept period.
                if usage["quantity"] > 3900:
                    has_long_record = True
            return has_long_record

        wait(check_expected_usage)

    @authors("ignat")
    def test_metering_profiling(self):
        create_pool(
            "my_pool",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 4},
                "abc": {"id": 1, "slug": "my", "name": "MyService"},
                "metering_tags": {"pool_tag": "pool_value"},
            },
            wait_for_orchid=False,
        )

        metering_count_sensor = profiler_factory().at_scheduler().counter("scheduler/metering/record_count")

        wait(lambda: metering_count_sensor.get_delta() > 0)

    @authors("ignat")
    def test_separate_schema_for_allocation(self):
        # NB(eshcherbin): Increase metering period to ensure accumulated usage value is averaged over the period.
        update_scheduler_config("resource_metering_period", 2000)
        update_scheduler_config("resource_metering/enable_separate_schema_for_allocation", True)
        set("//sys/pool_trees/default/@config/accumulated_resource_usage_update_period", 100)

        create_pool(
            "my_pool",
            pool_tree="default",
            attributes={
                "strong_guarantee_resources": {"cpu": 4},
                "abc": {"id": 1, "slug": "my", "name": "MyService"},
            },
            wait_for_orchid=False,
        )

        op = run_test_vanilla("sleep 2000", job_count=1, spec={"pool": "my_pool"})
        wait(lambda: op.get_job_count("running") == 1)

        root_key = (42, "default", "<Root>")

        desired_guarantees_metering_data = {
            root_key: {
                "strong_guarantee_resources/cpu": 0,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
            },
            (1, "default", "my_pool"): {
                "strong_guarantee_resources/cpu": 4,
                "resource_flow/cpu": 0,
                "burst_guarantee_resources/cpu": 0,
            },
        }

        desired_allocation_metering_data = {
            root_key: {
                "allocated_resources/cpu": 0,
            },
            (1, "default", "my_pool"): {
                "allocated_resources/cpu": 1.0,
            },
        }

        def check_expected_guarantee_records():
            event_key_to_last_record = self._extract_metering_records_from_log(schema="yt.scheduler.pools.compute_guarantee.v1")
            return self._validate_metering_records(root_key, desired_guarantees_metering_data, event_key_to_last_record)

        wait(check_expected_guarantee_records)

        def check_expected_allocation_records():
            event_key_to_last_record = self._extract_metering_records_from_log(schema="yt.scheduler.pools.compute_allocation.v1")
            # Update period equal 100ms, metering period is 1000ms, so we expected the error to be less than or equal to 10%.
            return self._validate_metering_records(root_key, desired_allocation_metering_data, event_key_to_last_record, precision=0.15)

        wait(check_expected_allocation_records)


##################################################################


class TestSchedulerObjectsDestruction(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {"heartbeat_period": 1},  # 1 msec
            "controller_agent_connector": {"heartbeat_period": 1},  # 1 msec
            "job_controller": {
                "resource_limits": {
                    "user_slots": 5,
                    "cpu": 5,
                    "memory": 5 * 1024 ** 3,
                },
            },
        },
    }

    @authors("pogorelov")
    def test_schedule_job_result_destruction(self):
        create_test_tables(row_count=100)

        try:
            map(
                command="""test $YT_JOB_INDEX -eq "1" && exit 1; cat""",
                in_="//tmp/t_in",
                out="//tmp/t_out",
                spec={
                    "data_size_per_job": 1,
                    "max_failed_job_count": 1
                },
            )
        except YtError:
            pass

        time.sleep(5)

        statistics = get("//sys/scheduler/orchid/monitoring/ref_counted/statistics")
        schedule_job_entry_object_type = \
            "NYT::NDetail::TPromiseState<NYT::TIntrusivePtr<NYT::NScheduler::TControllerScheduleJobResult> >"
        records = [record for record in statistics if record["name"] == schedule_job_entry_object_type]
        assert len(records) == 1

        assert records[0]["objects_alive"] == 0


##################################################################


class TestScheduleJobDelayAndRevive(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {"heartbeat_period": 1},  # 1 msec
            "controller_agent_connector": {"heartbeat_period": 1},  # 1 msec
        },
    }

    @authors("ignat")
    def test_schedule_job_delay(self):
        testing_options = {"schedule_job_delay": {"duration": 5000}}
        op = run_test_vanilla("sleep 2", job_count=2, spec={"testing": testing_options})

        wait(lambda: op.get_state() == "running")

        time.sleep(2)
        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        op.track()


##################################################################


class TestDelayInNodeHeartbeat(YTEnvSetup):
    # YT-17272
    # Scenario:
    # 1) Operation started.
    # 2) Node comes to scheduler with heartbeat and scheduler starts to process it.
    # 3) Node is banned.
    # 4) Scheduler aborts all jobs at node.
    # 5) New jobs are scheduled on the node, and scheduler replies to node.
    # 6) Node does not come to scheduler with heartbeats anymore since it is not connected to master.
    # 7) Scheduler does not abort job at node, since it is offline on scheduler and registration lease is long.
    # 8) Job hangs.

    NUM_MASTERS = 1
    NUM_NODES = 2
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {"heartbeat_period": 100},
            "controller_agent_connector": {"heartbeat_period": 10},
        },
    }

    @authors("pogorelov")
    def test_node_heartbeat_delay(self):
        def get_ongoing_heartbeats_count():
            ongoing_heartbeat_count_orchid_path = scheduler_orchid_path() + "/scheduler/node_shards/ongoing_heartbeat_count"
            return get(ongoing_heartbeat_count_orchid_path)

        nodes = ls("//sys/cluster_nodes")
        assert len(nodes) == 2

        first_node, second_node = nodes

        update_controller_agent_config("safe_online_node_count", 10000)

        # We want to control the moment we start schedule jobs on node.
        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(first_node), True)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(first_node)) == 0
        )

        set_banned_flag(True, nodes=[second_node], wait_for_scheduler=True)

        update_scheduler_config("testing_options/node_heartbeat_processing_delay", {
            "duration": 3000,
            "type": "async",
        })

        op = run_test_vanilla("sleep 5", job_count=1)

        # Scheduler starts making a delay in heartbeat processing here.
        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(first_node), False)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(first_node)) > 0
        )

        # We want to ban node during delay in heartbeat.
        wait(lambda: get_ongoing_heartbeats_count() > 0)
        assert get_ongoing_heartbeats_count() == 1

        # Increase period to controller agent do not know that node is not online anymore.
        update_controller_agent_config("exec_nodes_update_period", 5000)

        print_debug("Ban node", first_node)
        set_banned_flag(True, nodes=[first_node], wait_for_scheduler=True)

        # We want to unban nodes only when heartbeat processing of the banned node is finished.
        wait(lambda: get_ongoing_heartbeats_count() == 0)

        update_controller_agent_config("exec_nodes_update_period", 100)

        update_scheduler_config("testing_options/node_heartbeat_processing_delay", {
            "duration": 0,
            "type": "sync",
        })

        set_banned_flag(False, nodes=[second_node], wait_for_scheduler=True)

        op.track()
