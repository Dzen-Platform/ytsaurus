import pytest
from flaky import flaky

from yt_env_setup import YTEnvSetup, unix_only, require_ytserver_root_privileges, wait
from yt_commands import *

import string
import time

##################################################################

class TestSchedulerAlerts(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "alerts_update_period": 100,
            "watchers_update_period": 100,
            "fair_share_update_period": 100,
        }
    }

    def test_pools(self):
        assert get("//sys/scheduler/@alerts") == []

        # Incorrect pool configuration.
        create("map_node", "//sys/pools/poolA", attributes={"min_share_ratio": 2.0})

        time.sleep(0.5)
        assert len(get("//sys/scheduler/@alerts")) == 1

        set("//sys/pools/poolA/@min_share_ratio", 0.8)

        time.sleep(0.5)
        assert get("//sys/scheduler/@alerts") == []

        # Total min_share_ratio > 1.
        create("map_node", "//sys/pools/poolB", attributes={"min_share_ratio": 0.8})

        time.sleep(0.5)
        assert len(get("//sys/scheduler/@alerts")) == 1

        set("//sys/pools/poolA/@min_share_ratio", 0.1)

        time.sleep(0.5)
        assert get("//sys/scheduler/@alerts") == []

    def test_config(self):
        assert get("//sys/scheduler/@alerts") == []

        set("//sys/scheduler/config", {"fair_share_update_period": -100})

        time.sleep(0.5)
        assert len(get("//sys/scheduler/@alerts")) == 1

        set("//sys/scheduler/config", {})

        time.sleep(0.5)
        assert get("//sys/scheduler/@alerts") == []

    def test_cluster_directory(self):
        assert get("//sys/scheduler/@alerts") == []

        set("//sys/clusters/banach", {})

        time.sleep(1.0)
        assert len(get("//sys/scheduler/@alerts")) == 1

        set("//sys/clusters", {})

        time.sleep(1.0)
        assert get("//sys/scheduler/@alerts") == []

##################################################################

class TestSchedulerOperationAlerts(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_SCHEDULERS = 1
    NUM_NODES = 3

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {
                "heartbeat_period": 200
            },
            "slot_manager": {
                "job_environment": {
                    "type": "cgroups",
                    "supported_cgroups": ["blkio"],
                    "block_io_watchdog_period": 100
                }
            }
        }
    }

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "operation_progress_analysis_period": 200,
            "operations_update_period": 100,
            "operation_alerts": {
                "tmpfs_alert_min_unused_space_threshold": 200,
                "tmpfs_alert_max_unused_space_ratio": 0.3,
                "aborted_jobs_alert_max_aborted_time": 100,
                "aborted_jobs_alert_aborted_time_ratio": 0.05,
                "intermediate_data_skew_alert_min_interquartile_range": 50,
                "intermediate_data_skew_alert_min_partition_size": 50,
                "short_jobs_alert_min_job_count": 3,
                "short_jobs_alert_min_job_duration": 5000
            },
            "iops_threshold": 50,
            "map_reduce_operation_options": {
                "min_uncompressed_block_size": 1
            },
            "schedule_job_time_limit": 3000,
        }
    }

    @require_ytserver_root_privileges
    @unix_only
    def test_unused_tmpfs_size_alert(self):
        create("table", "//tmp/t_input")
        create("table", "//tmp/t_output")
        write_table("//tmp/t_input", [{"x": "y"}])

        op = map(
            command="echo abcdef >local_file; sleep 1.5; cat",
            in_="//tmp/t_input",
            out="//tmp/t_output",
            spec={
                "mapper": {
                    "tmpfs_size": 5 * 1024 * 1024,
                    "tmpfs_path": "."
                }
            })

        assert "unused_tmpfs_space" in get("//sys/operations/{0}/@alerts".format(op.id))

        op = map(
            command="printf '=%.0s' {1..768} >local_file; sleep 1.5; cat",
            in_="//tmp/t_input",
            out="//tmp/t_output",
            spec={
                "mapper": {
                    "tmpfs_size": 1024,
                    "tmpfs_path": "."
                }
            })

        assert "unused_tmpfs_space" not in get("//sys/operations/{0}/@alerts".format(op.id))

    def test_missing_input_chunks_alert(self):
        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out")
        write_table("//tmp/t_in", [{"x": "y"}])

        chunk_ids = get("//tmp/t_in/@chunk_ids")
        assert len(chunk_ids) == 1

        replicas = get("#{0}/@stored_replicas".format(chunk_ids[0]))
        set_banned_flag(True, replicas)

        op = map(
            command="sleep 1.5; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "unavailable_chunk_strategy": "wait",
                "unavailable_chunk_tactics": "wait"
            },
            dont_track=True)

        time.sleep(1.0)
        assert "lost_input_chunks" in get("//sys/operations/{0}/@alerts".format(op.id))

        set_banned_flag(False, replicas)
        time.sleep(1.0)

        assert "lost_input_chunks" not in get("//sys/operations/{0}/@alerts".format(op.id))

    @pytest.mark.skipif("True", reason="YT-6717")
    def test_woodpecker_jobs_alert(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"x": str(i)} for i in xrange(7)])
        create("table", "//tmp/t_out")

        cmd = "set -e; echo aaa >local_file; for i in {1..200}; do " \
              "dd if=./local_file of=/dev/null iflag=direct bs=1M count=1; done;"

        op = map(
            command=cmd,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "data_size_per_job": 1,
                "resource_limits": {
                    "user_slots": 1
                }
            })

        assert "excessive_disk_usage" in get("//sys/operations/{0}/@alerts".format(op.id))

    @flaky(max_runs=3)
    def test_long_aborted_jobs_alert(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"x": str(i)} for i in xrange(5)])
        create("table", "//tmp/t_out")

        op = map(
            command="sleep 100; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "data_size_per_job": 1,
            },
            dont_track=True)

        operation_orchid_path = "//sys/scheduler/orchid/scheduler/operations/" + op.id
        running_jobs_count_path = operation_orchid_path + "/progress/jobs/running"

        def running_jobs_exists():
            return exists(running_jobs_count_path) and get(running_jobs_count_path) >= 1

        wait(running_jobs_exists)

        for job in ls(operation_orchid_path + "/running_jobs"):
            abort_job(job)

        time.sleep(1.5)

        assert "long_aborted_jobs" in get("//sys/operations/{0}/@alerts".format(op.id))

        abort_op(op.id)

    def test_intermediate_data_skew_alert(self):
        create("table", "//tmp/t_in")

        mutliplier = 1
        data = []
        for letter in ["a", "b", "c", "d", "e"]:
            data.extend([{"x": letter} for _ in xrange(mutliplier)])
            mutliplier *= 10

        write_table("//tmp/t_in", data)

        create("table", "//tmp/t_out")

        op = map_reduce(
            mapper_command="cat",
            reducer_command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            sort_by=["x"],
            spec={"partition_count": 5})

        assert "intermediate_data_skew" in get("//sys/operations/{0}/@alerts".format(op.id))

    @flaky(max_runs=3)
    def test_short_jobs_alert(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"x": str(i)} for i in xrange(4)])
        create("table", "//tmp/t_out")

        op = map(
            command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "data_size_per_job": 1,
            })

        assert "short_jobs_duration" in get("//sys/operations/{0}/@alerts".format(op.id))

        op = map(
            command="sleep 5; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "data_size_per_job": 1,
            })

        assert "short_jobs_duration" not in get("//sys/operations/{0}/@alerts".format(op.id))

    def test_schedule_job_timed_out_alert(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"x": "y"}])
        create("table", "//tmp/t_out")

        testing_options = {"scheduling_delay": 3500, "scheduling_delay_type": "async"}

        op = map(
            command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "testing": testing_options,
            },
            dont_track=True)

        time.sleep(8)

        assert "schedule_job_timed_out" in get("//sys/operations/{0}/@alerts".format(op.id))

        op.abort()

##################################################################

class TestSchedulerJobSpecThrottlerOperationAlert(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "operation_progress_analysis_period": 100,
            "operations_update_period": 100,
            "heavy_job_spec_slice_count_threshold": 1,
            "job_spec_slice_throttler": {
                "limit": 3,
                "period": 1000
            },
            "operation_alerts": {
                "job_spec_throttling_alert_activation_count_threshold": 1
            }
        }
    }

    def test_job_spec_throttler_operation_alert(self):
        create("table", "//tmp/t_in")
        for letter in string.ascii_lowercase:
            write_table("<append=%true>//tmp/t_in", [{"x": letter}])

        create("table", "//tmp/t_out")
        create("table", "//tmp/t_out2")

        op1 = map(
            command="sleep 100; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            dont_track=True)

        op2 = map(
            command="sleep 1; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out2",
            dont_track=True)

        time.sleep(1.5)

        assert "excessive_job_spec_throttling" in get("//sys/operations/{0}/@alerts".format(op2.id))

        op1.abort()
        op2.abort()
