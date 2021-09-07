from yt_env_setup import (
    YTEnvSetup,
    Restarter,
    is_asan_build,
    SCHEDULERS_SERVICE,
    CONTROLLER_AGENTS_SERVICE,
)

from yt_commands import (
    authors, print_debug, wait, wait_breakpoint, release_breakpoint, with_breakpoint, create,
    ls, get,
    set, remove, exists, create_account, create_tmpdir, create_user, create_pool, start_transaction, abort_transaction,
    read_table, write_table, map, sort,
    run_test_vanilla, run_sleeping_vanilla,
    abort_job, get_job, get_job_fail_context,
    abandon_job, get_operation_cypress_path, sync_create_cells, update_controller_agent_config, update_scheduler_config,
    set_banned_flag, PrepareTables, get_statistics)

from yt_scheduler_helpers import scheduler_orchid_pool_path, scheduler_orchid_default_pool_tree_path

from yt_helpers import get_current_time, parse_yt_time
from yt.test_helpers.profiler import Profiler, get_job_count_profiling

from yt.yson import YsonEntity
from yt.common import YtResponseError, YtError
from yt.test_helpers import are_almost_equal
import yt.environment.init_operation_archive as init_operation_archive
from yt.environment import arcadia_interop

import pytest
from flaky import flaky

import shutil
import time
import subprocess
import os.path
from datetime import timedelta

##################################################################


class TestSchedulerFunctionality(YTEnvSetup, PrepareTables):
    NUM_TEST_PARTITIONS = 4
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "connect_retry_backoff_time": 100,
            "fair_share_update_period": 100,
            "profiling_update_period": 100,
            "fair_share_profiling_period": 100,
            "alerts_update_period": 100,
            # Unrecognized alert often interferes with the alerts that
            # are tested in this test suite.
            "enable_unrecognized_alert": False,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operation_time_limit_check_period": 100,
            "operation_controller_fail_timeout": 3000,
        }
    }

    USE_PORTO = True

    @authors("ignat")
    def test_connection_time(self):
        connection_time_attr = parse_yt_time(get("//sys/scheduler/@connection_time"))
        connection_time_orchid = parse_yt_time(get("//sys/scheduler/orchid/scheduler/service/last_connection_time"))
        assert connection_time_orchid - connection_time_attr < timedelta(seconds=2)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        new_connection_time_attr = parse_yt_time(get("//sys/scheduler/@connection_time"))
        new_connection_time_orchid = parse_yt_time(get("//sys/scheduler/orchid/scheduler/service/last_connection_time"))

        assert new_connection_time_attr > connection_time_attr
        assert new_connection_time_orchid > connection_time_orchid

    @authors("ignat")
    @flaky(max_runs=3)
    def test_revive(self):
        def get_connection_time():
            return parse_yt_time(get("//sys/scheduler/@connection_time"))

        self._prepare_tables()

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo '{foo=bar}'; sleep 4",
        )

        time.sleep(3)

        assert get_current_time() - get_connection_time() > timedelta(seconds=3)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        assert get_current_time() - get_connection_time() < timedelta(seconds=3)

        op.track()

        assert read_table("//tmp/t_out") == [{"foo": "bar"}]

    @authors("ignat")
    def test_banned_operation(self):
        self._prepare_tables()

        self._create_table("//tmp/t_out1")
        self._create_table("//tmp/t_out2")

        op1 = map(track=False, in_="//tmp/t_in", out="//tmp/t_out1", command="sleep 1000")
        op2 = map(track=False, in_="//tmp/t_in", out="//tmp/t_out2", command="sleep 1000")
        op1.ensure_running()
        op2.ensure_running()

        set(op1.get_path() + "/@banned", True)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        wait(lambda: ls("//sys/scheduler/orchid/scheduler/operations") == [op2.id])

    @authors("ignat")
    def test_disconnect_during_revive(self):
        op_count = 20

        self._create_table("//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})
        for i in xrange(1, op_count + 1):
            self._create_table("//tmp/t_out" + str(i))

        ops = []
        for i in xrange(1, op_count):
            ops.append(
                map(
                    track=False,
                    # Sleep is necessary since we not support revive for completing operations.
                    command="sleep 3; cat",
                    in_=["//tmp/t_in"],
                    out="//tmp/t_out" + str(i),
                )
            )

        for i in range(10):
            while True:
                scheduler_locks = get("//sys/scheduler/lock/@locks", verbose=False)
                if len(scheduler_locks) > 0:
                    scheduler_transaction = scheduler_locks[0]["transaction_id"]
                    abort_transaction(scheduler_transaction)
                    break
                time.sleep(0.01)

        for op in ops:
            op.track()

        for i in xrange(1, op_count):
            assert read_table("//tmp/t_out" + str(i)) == [{"foo": "bar"}]

    @authors("ignat")
    def test_user_transaction_abort_when_scheduler_is_down(self):
        self._prepare_tables()

        transaction_id = start_transaction(timeout=300 * 1000)
        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo '{foo=bar}'; sleep 50",
            transaction_id=transaction_id,
        )

        wait(lambda: op.get_job_count("running") == 1)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            abort_transaction(transaction_id)

        with pytest.raises(YtError):
            op.track()

    @authors("ignat")
    def test_scheduler_transaction_abort_when_scheduler_is_down(self):
        self._prepare_tables()

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo '{foo=bar}'; sleep 3",
        )

        time.sleep(2)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            abort_transaction(get(op.get_path() + "/@input_transaction_id"))
            abort_transaction(get(op.get_path() + "/@output_transaction_id"))

        op.track()

        assert read_table("//tmp/t_out") == [{"foo": "bar"}]

    @authors("ignat")
    def test_scheduler_operation_abort_when_operation_is_orphaned(self):
        self._prepare_tables()

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo '{foo=bar}'; sleep 100",
        )

        wait(lambda: op.get_state() == "running")

        input_tx = get(op.get_path() + "/@input_transaction_id")
        assert exists("#" + input_tx)

        update_scheduler_config("testing_options/handle_orphaned_operations_delay", 5000)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            assert exists("#" + input_tx)

        wait(lambda: op.get_state() == "orphaned")

        op.abort()

        assert not exists("#" + input_tx)

    @authors("ignat")
    def test_suspend_during_revive(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")
        write_table("//tmp/in", [{"foo": i} for i in xrange(5)])

        op = map(track=False, command="sleep 1000", in_=["//tmp/in"], out="//tmp/out")
        op.ensure_running()

        op.suspend()
        wait(lambda: get(op.get_path() + "/@suspended"))

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        op.ensure_running()

        wait(lambda: op.get_job_count("running") == 0)

        assert get(op.get_path() + "/@suspended")

        op.resume()
        wait(lambda: op.get_job_count("running") == 1)

    @authors("ignat")
    def test_operation_time_limit(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")

        write_table("//tmp/in", [{"foo": i} for i in xrange(5)])

        # Operation specific time limit.
        op2 = map(
            track=False,
            command="sleep 3.0; cat >/dev/null",
            in_=["//tmp/in"],
            out="//tmp/out",
            spec={"time_limit": 1000},
        )

        wait(lambda: op2.get_state() == "failed")

    @authors("ignat")
    def test_operation_suspend_with_account_limit_exceeded(self):
        create_account("limited")
        set("//sys/accounts/limited/@resource_limits/chunk_count", 1)

        self._create_table("//tmp/in")
        self._create_table("//tmp/out")
        set("//tmp/out/@account", "limited")
        write_table("//tmp/in", [{"foo": i} for i in xrange(3)])

        op = map(
            track=False,
            command="sleep $YT_JOB_INDEX; cat",
            in_=["//tmp/in"],
            out="//tmp/out",
            spec={
                "data_size_per_job": 1,
                "suspend_operation_if_account_limit_exceeded": True,
            },
        )

        wait(lambda: get(op.get_path() + "/@suspended"), iter=100, sleep_backoff=0.6)

        time.sleep(0.5)

        assert op.get_state() == "running"

        alerts = get(op.get_path() + "/@alerts")
        assert list(alerts) == ["operation_suspended"]

        set("//sys/accounts/limited/@resource_limits/chunk_count", 10)
        op.resume()
        op.track()

        assert op.get_state() == "completed"
        assert not get(op.get_path() + "/@suspended")
        assert not get(op.get_path() + "/@alerts")

    @authors("max42")
    def test_suspend_operation_after_materialization(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")
        write_table("//tmp/in", [{"foo": 0}])

        op = map(
            track=False,
            command="cat",
            in_="//tmp/in",
            out="//tmp/out",
            spec={
                "data_size_per_job": 1,
                "suspend_operation_after_materialization": True,
            },
        )
        wait(lambda: get(op.get_path() + "/@suspended"))
        op.resume()
        op.track()

    @authors("ignat")
    def test_fail_context_saved_on_time_limit(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out")

        write_table("//tmp/in", [{"foo": i} for i in xrange(5)])

        op = map(
            track=False,
            command="sleep 1000.0; cat >/dev/null",
            in_=["//tmp/in"],
            out="//tmp/out",
            spec={"time_limit": 2000},
        )

        wait(lambda: op.get_state() == "failed")
        wait(lambda: op.list_jobs())

        jobs = op.list_jobs()

        for job_id in jobs:
            assert len(get_job_fail_context(op.id, job_id)) > 0

    # Test is flaky by the next reason: schedule job may fail by some reason (chunk list demand is not met, et.c)
    # and in this case we can successfully schedule job for the next operation in queue.
    @authors("ignat")
    @flaky(max_runs=3)
    def test_fifo_default(self):
        self._create_table("//tmp/in")
        self._create_table("//tmp/out1")
        self._create_table("//tmp/out2")
        self._create_table("//tmp/out3")
        write_table("//tmp/in", [{"foo": i} for i in xrange(5)])

        create_pool("fifo_pool", ignore_existing=True)
        set("//sys/pools/fifo_pool/@mode", "fifo")

        pools_orchid = scheduler_orchid_default_pool_tree_path() + "/pools"
        wait(lambda: exists(pools_orchid + "/fifo_pool"))
        wait(lambda: get(pools_orchid + "/fifo_pool/mode") == "fifo")

        ops = []
        for i in xrange(1, 4):
            ops.append(
                map(
                    track=False,
                    command="sleep 3; cat >/dev/null",
                    in_=["//tmp/in"],
                    out="//tmp/out" + str(i),
                    spec={"pool": "fifo_pool"},
                )
            )

        for op in ops:
            op.track()

        finish_times = [get(op.get_path() + "/@finish_time") for op in ops]
        for cur, next in zip(finish_times, finish_times[1:]):
            assert cur < next

    # Test is flaky by the next reason: schedule job may fail by some reason (chunk list demand is not met, et.c)
    # and in this case we can successfully schedule job for the next operation in queue.
    @authors("ignat")
    @flaky(max_runs=3)
    def test_fifo_by_pending_job_count(self):
        op_count = 3

        for i in xrange(1, op_count + 1):
            self._create_table("//tmp/in" + str(i))
            self._create_table("//tmp/out" + str(i))
            write_table(
                "//tmp/in" + str(i),
                [{"foo": j} for j in xrange(op_count * (op_count + 1 - i))],
            )

        create_pool("fifo_pool", ignore_existing=True)
        set("//sys/pools/fifo_pool/@mode", "fifo")
        set("//sys/pools/fifo_pool/@fifo_sort_parameters", ["pending_job_count"])

        # Wait until pools tree would be updated
        time.sleep(0.6)

        ops = []
        for i in xrange(1, op_count + 1):
            ops.append(
                map(
                    track=False,
                    command="sleep 2.0; cat >/dev/null",
                    in_=["//tmp/in" + str(i)],
                    out="//tmp/out" + str(i),
                    spec={"pool": "fifo_pool", "data_size_per_job": 1},
                )
            )

        time.sleep(1.0)
        for index, op in enumerate(ops):
            assert op.get_runtime_progress("scheduling_info_per_pool_tree/default/fifo_index") == 2 - index

        for op in ops:
            op.track()

        finish_times = [get(op.get_path() + "/@finish_time") for op in ops]
        for cur, next in zip(finish_times, finish_times[1:]):
            assert cur > next

    @authors("ignat")
    def test_preparing_operation_transactions(self):
        self._prepare_tables()

        set_banned_flag(True)
        op = sort(track=False, in_="//tmp/t_in", out="//tmp/t_in", sort_by=["foo"])
        time.sleep(2)

        for tx in ls("//sys/transactions", attributes=["operation_id"]):
            if tx.attributes.get("operation_id", "") == op.id:
                for i in xrange(10):
                    try:
                        abort_transaction(tx)
                    except YtResponseError as err:
                        if err.is_no_such_transaction():
                            break
                        if i == 9:
                            raise

        with pytest.raises(YtError):
            op.track()

        set_banned_flag(False)

    @authors("ignat")
    def test_abort_custom_error_message(self):
        self._prepare_tables()

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo '{foo=bar}'; sleep 3",
        )
        op.abort(abort_message="Test abort")

        assert op.get_state() == "aborted"
        assert get(op.get_path() + "/@result/error/inner_errors/0/message") == "Test abort"

    @authors("ignat")
    def test_operation_pool_attributes(self):
        self._prepare_tables()

        op = map(in_="//tmp/t_in", out="//tmp/t_out", command="cat")
        assert get(op.get_path() + "/@runtime_parameters/scheduling_options_per_pool_tree/default/pool") == "root"

    @authors("babenko", "gritukan")
    def test_operation_events_attribute(self):
        op = run_test_vanilla(with_breakpoint("BREAKPOINT"))

        wait_breakpoint()

        events = get(op.get_path() + "/@events")
        assert [
            "starting",
            "waiting_for_agent",
            "initializing",
            "preparing",
            "pending",
            "materializing",
            "running",
        ] == [event["state"] for event in events]

        def event_contains_agent_address(event):
            if "controller_agent_address" not in event["attributes"]:
                return False
            return len(event["attributes"]["controller_agent_address"]) > 0

        assert any(event_contains_agent_address(event) for event in events)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        wait(lambda: len(get(op.get_path() + "/@events")) == 14)
        events = get(op.get_path() + "/@events")
        assert [
            "starting",
            "waiting_for_agent",
            "initializing",
            "preparing",
            "pending",
            "materializing",
            "running",
            "orphaned",
            "waiting_for_agent",
            "revive_initializing",
            "reviving",
            "pending",
            "materializing",
            "running",
        ] == [event["state"] for event in events]
        assert sum(event_contains_agent_address(event) for event in events) == 2

        release_breakpoint()
        op.track()

        events = get(op.get_path() + "/@events")
        assert [
            "starting",
            "waiting_for_agent",
            "initializing",
            "preparing",
            "pending",
            "materializing",
            "running",
            "orphaned",
            "waiting_for_agent",
            "revive_initializing",
            "reviving",
            "pending",
            "materializing",
            "running",
            "completing",
            "completed",
        ] == [event["state"] for event in events]
        assert sum(event_contains_agent_address(event) for event in events) == 2

    @authors("ignat")
    def test_exceed_job_time_limit(self):
        self._prepare_tables()

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="sleep 3 ; cat",
            spec={"max_failed_job_count": 1, "mapper": {"job_time_limit": 2000}},
        )

        # if all jobs failed then operation is also failed
        with pytest.raises(YtError):
            op.track()

        for job_id in op.list_jobs():
            inner_errors = get_job(op.id, job_id)["error"]["inner_errors"]
            assert "Job time limit exceeded" in inner_errors[0]["message"]

    @authors("ignat")
    @flaky(max_runs=3)
    def test_within_job_time_limit(self):
        self._prepare_tables()
        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="sleep 1 ; cat",
            spec={"max_failed_job_count": 1, "mapper": {"job_time_limit": 3000}},
        )

    @authors("ignat")
    def test_suspend_resume(self):
        self._create_table("//tmp/t_in")
        self._create_table("//tmp/t_out")
        write_table("//tmp/t_in", [{"foo": i} for i in xrange(10)])

        op = map(
            track=False,
            command="sleep 1; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"data_size_per_job": 1},
        )

        for i in xrange(5):
            time.sleep(0.5)
            op.suspend(abort_running_jobs=True)
            time.sleep(0.5)
            op.resume()

        for i in xrange(5):
            op.suspend()
            op.resume()

        for i in xrange(5):
            op.suspend(abort_running_jobs=True)
            op.resume()

        op.track()

        assert sorted(read_table("//tmp/t_out")) == [{"foo": i} for i in xrange(10)]

    @authors("ignat")
    def test_table_changed_during_operation_prepare(self):
        self._prepare_tables()

        op1 = map(
            track=False,
            in_="//tmp/t_in",
            out="<append=true>//tmp/t_in",
            command="cat",
            spec={
                "testing": {
                    "delay_inside_prepare": 5000,
                }
            },
        )
        wait(lambda: op1.get_state() == "completed")

        assert sorted(read_table("//tmp/t_in")) == [{"foo": "bar"} for _ in xrange(2)]

        op2 = map(
            track=False,
            in_="//tmp/t_in",
            out="<append=true>//tmp/t_in",
            command="cat",
            spec={
                "testing": {
                    "delay_inside_prepare": 5000,
                }
            },
        )
        wait(lambda: get("//tmp/t_in/@locks"))
        write_table("<append=true>//tmp/t_in", [{"x": "y"}])
        wait(lambda: op2.get_state() == "failed")

        op3 = map(
            track=False,
            in_="//tmp/t_in",
            out="<append=true>//tmp/t_in",
            command="cat",
            spec={
                "testing": {
                    "delay_inside_prepare": 5000,
                }
            },
        )
        wait(lambda: get("//tmp/t_in/@locks"))
        write_table("//tmp/t_in", [{"x": "y"}])
        wait(lambda: op3.get_state() == "failed")

    @authors("ignat")
    def test_starting_operation(self):
        self._prepare_tables()

        op_response = map(
            track=False,
            in_="//tmp/t_in",
            out="<append=true>//tmp/t_in",
            command="cat",
            spec={
                "testing": {
                    "delay_before_start": 5000,
                }
            },
            return_response=True,
        )

        time.sleep(2)

        scheduler_locks = get("//sys/scheduler/lock/@locks", verbose=False)
        assert len(scheduler_locks) > 0
        scheduler_transaction = scheduler_locks[0]["transaction_id"]
        abort_transaction(scheduler_transaction)

        op_response.wait()
        assert not op_response.is_ok()
        error = YtResponseError(op_response.error())
        assert error.contains_text("Master disconnected")

    @authors("gritukan")
    def test_operation_abort_failed(self):
        self._prepare_tables()

        connection_time = parse_yt_time(get("//sys/scheduler/@connection_time"))

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="echo '{foo=bar}'; sleep 10000000",
            spec={"testing": {"throw_exception_during_operation_abort": True}},
        )
        wait(lambda: op.get_job_count("running") == 1)

        op.abort()
        wait(lambda: op.get_state() == "aborted")

        # Scheduler should not reconnect.
        new_connection_time = parse_yt_time(get("//sys/scheduler/@connection_time"))
        assert new_connection_time == connection_time


class TestSchedulerProfiling(YTEnvSetup, PrepareTables):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "connect_retry_backoff_time": 100,
            "fair_share_update_period": 100,
            "profiling_update_period": 100,
            "fair_share_profiling_period": 100,
            "alerts_update_period": 100,
            # Unrecognized alert often interferes with the alerts that
            # are tested in this test suite.
            "enable_unrecognized_alert": False,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operation_time_limit_check_period": 100,
            "operation_controller_fail_timeout": 3000,
        }
    }

    USE_PORTO = True

    @authors("ignat", "eshcherbin")
    def test_pool_profiling(self):
        create_pool("unique_pool")
        pool_path = "//sys/pools/unique_pool"
        set(pool_path + "/@max_operation_count", 50)
        wait(lambda: get(scheduler_orchid_pool_path("unique_pool") + "/max_operation_count") == 50)
        set(pool_path + "/@max_running_operation_count", 8)
        wait(lambda: get(scheduler_orchid_pool_path("unique_pool") + "/max_running_operation_count") == 8)

        profiler = Profiler.at_scheduler(self.Env.create_native_client(), fixed_tags={"tree": "default", "pool": "unique_pool"})

        metric_prefix = "scheduler/pools/"
        dominant_fair_share_sensor = profiler.gauge(metric_prefix + "dominant_fair_share/total")
        dominant_usage_share_sensor = profiler.gauge(metric_prefix + "dominant_usage_share")
        dominant_demand_share_sensor = profiler.gauge(metric_prefix + "dominant_demand_share")
        dominant_promised_fair_share_sensor = profiler.gauge(metric_prefix + "promised_dominant_fair_share")
        cpu_usage_sensor = profiler.gauge(metric_prefix + "resource_usage/cpu")
        user_slots_usage_sensor = profiler.gauge(metric_prefix + "resource_usage/user_slots")
        cpu_demand_sensor = profiler.gauge(metric_prefix + "resource_demand/cpu")
        user_slots_demand_sensor = profiler.gauge(metric_prefix + "resource_demand/user_slots")
        running_operation_count_sensor = profiler.gauge(metric_prefix + "running_operation_count")
        total_operation_count_sensor = profiler.gauge(metric_prefix + "total_operation_count")
        max_operation_count_sensor = profiler.gauge(metric_prefix + "max_operation_count")
        max_running_operation_count_sensor = profiler.gauge(metric_prefix + "max_running_operation_count")
        finished_operation_count_sensor = profiler.gauge(metric_prefix + "finished_operation_count")
        strong_guarantee_resources_cpu_sensor = profiler.gauge(metric_prefix + "strong_guarantee_resources/cpu")
        strong_guarantee_resources_memory_sensor = profiler.gauge(metric_prefix + "strong_guarantee_resources/user_memory")
        strong_guarantee_resources_user_slots_sensor = profiler.gauge(metric_prefix + "strong_guarantee_resources/user_slots")

        op = run_sleeping_vanilla(spec={"pool": "unique_pool"})

        wait(lambda: dominant_fair_share_sensor.get() == 1.0)
        wait(lambda: dominant_usage_share_sensor.get() == 1.0)
        wait(lambda: dominant_demand_share_sensor.get() == 1.0)
        wait(lambda: dominant_promised_fair_share_sensor.get() == 1.0)
        wait(lambda: cpu_usage_sensor.get() == 1)
        wait(lambda: user_slots_usage_sensor.get() == 1)
        wait(lambda: cpu_demand_sensor.get() == 1)
        wait(lambda: user_slots_demand_sensor.get() == 1)
        wait(lambda: running_operation_count_sensor.get() == 1)
        wait(lambda: total_operation_count_sensor.get() == 1)

        # pool guaranties metrics
        wait(lambda: max_operation_count_sensor.get() == 50)
        wait(lambda: max_running_operation_count_sensor.get() == 8)
        wait(lambda: strong_guarantee_resources_cpu_sensor.get() == 0)
        wait(lambda: strong_guarantee_resources_memory_sensor.get() == 0)
        wait(lambda: strong_guarantee_resources_user_slots_sensor.get() == 0)
        op.complete()

        wait(lambda: finished_operation_count_sensor.get(tags={"state": "completed"}) == 1)
        wait(lambda: finished_operation_count_sensor.get(tags={"state": "failed"}) == 0)
        wait(lambda: finished_operation_count_sensor.get(tags={"state": "aborted"}) == 0)

    @authors("ignat", "eshcherbin")
    def test_operations_by_slot_profiling(self):
        create_pool("some_pool")

        profiler = Profiler.at_scheduler(self.Env.create_native_client(), fixed_tags={"tree": "default", "pool": "some_pool"})

        metric_prefix = "scheduler/operations_by_slot/"
        dominant_fair_share_sensor = profiler.gauge(metric_prefix + "dominant_fair_share/total")
        dominant_usage_share_sensor = profiler.gauge(metric_prefix + "dominant_usage_share")
        dominant_demand_share_sensor = profiler.gauge(metric_prefix + "dominant_demand_share")
        dominant_promised_fair_share_sensor = profiler.gauge(metric_prefix + "promised_dominant_fair_share")
        cpu_usage_sensor = profiler.gauge(metric_prefix + "resource_usage/cpu")
        user_slots_usage_sensor = profiler.gauge(metric_prefix + "resource_usage/user_slots")
        cpu_demand_sensor = profiler.gauge(metric_prefix + "resource_demand/cpu")
        user_slots_demand_sensor = profiler.gauge(metric_prefix + "resource_demand/user_slots")

        op1 = run_sleeping_vanilla(spec={"pool": "some_pool"})
        wait(lambda: op1.get_job_count("running") == 1)
        op2 = run_sleeping_vanilla(spec={"pool": "some_pool"})
        op2.wait_for_state("running")

        def get_slot_index(op):
            return op.get_runtime_progress("scheduling_info_per_pool_tree/default/slot_index", default=-1)

        wait(lambda: get_slot_index(op1) == 0)
        wait(lambda: get_slot_index(op2) == 1)

        wait(lambda: are_almost_equal(dominant_fair_share_sensor.get(tags={"slot_index": "0"}), 0.5))
        wait(lambda: dominant_usage_share_sensor.get(tags={"slot_index": "0"}) == 1.0)
        wait(lambda: dominant_demand_share_sensor.get(tags={"slot_index": "0"}) == 1.0)
        wait(lambda: are_almost_equal(dominant_promised_fair_share_sensor.get(tags={"slot_index": "0"}), 0.5))
        wait(lambda: cpu_usage_sensor.get(tags={"slot_index": "0"}) == 1)
        wait(lambda: user_slots_usage_sensor.get(tags={"slot_index": "0"}) == 1)
        wait(lambda: cpu_demand_sensor.get(tags={"slot_index": "0"}) == 1)
        wait(lambda: user_slots_demand_sensor.get(tags={"slot_index": "0"}) == 1)

        wait(lambda: are_almost_equal(dominant_fair_share_sensor.get(tags={"slot_index": "1"}), 0.5))
        wait(lambda: dominant_usage_share_sensor.get(tags={"slot_index": "1"}) == 0)
        wait(lambda: dominant_demand_share_sensor.get(tags={"slot_index": "1"}) == 1.0)
        wait(lambda: are_almost_equal(dominant_promised_fair_share_sensor.get(tags={"slot_index": "1"}), 0.5))
        wait(lambda: cpu_usage_sensor.get(tags={"slot_index": "1"}) == 0)
        wait(lambda: user_slots_usage_sensor.get(tags={"slot_index": "1"}) == 0)
        wait(lambda: cpu_demand_sensor.get(tags={"slot_index": "1"}) == 1)
        wait(lambda: user_slots_demand_sensor.get(tags={"slot_index": "1"}) == 1)

        op1.abort(wait_until_finished=True)

        wait(lambda: dominant_fair_share_sensor.get(tags={"slot_index": "1"}) == 1.0)
        wait(lambda: dominant_usage_share_sensor.get(tags={"slot_index": "1"}) == 1.0)
        wait(lambda: dominant_demand_share_sensor.get(tags={"slot_index": "1"}) == 1.0)
        wait(lambda: dominant_promised_fair_share_sensor.get(tags={"slot_index": "1"}) == 1.0)

    @authors("ignat", "eshcherbin")
    def test_operations_by_user_profiling(self):
        create_user("ignat")
        create_user("egor")

        create_pool("some_pool")
        create_pool("other_pool", attributes={"allowed_profiling_tags": ["hello", "world"]})

        profiler = Profiler.at_scheduler(self.Env.create_native_client(), fixed_tags={"tree": "default"})

        metric_prefix = "scheduler/operations_by_user/"
        dominant_fair_share_sensor = profiler.gauge(metric_prefix + "dominant_fair_share/total")
        dominant_usage_share_sensor = profiler.gauge(metric_prefix + "dominant_usage_share")
        dominant_demand_share_sensor = profiler.gauge(metric_prefix + "dominant_demand_share")
        dominant_promised_fair_share_sensor = profiler.gauge(metric_prefix + "promised_dominant_fair_share")
        cpu_usage_sensor = profiler.gauge(metric_prefix + "resource_usage/cpu")
        user_slots_usage_sensor = profiler.gauge(metric_prefix + "resource_usage/user_slots")
        cpu_demand_sensor = profiler.gauge(metric_prefix + "resource_demand/cpu")
        user_slots_demand_sensor = profiler.gauge(metric_prefix + "resource_demand/user_slots")

        op1 = run_sleeping_vanilla(
            spec={"pool": "some_pool", "custom_profiling_tag": "hello"},
            authenticated_user="ignat",
        )
        wait(lambda: op1.get_job_count("running") == 1)
        op2 = run_sleeping_vanilla(
            spec={"pool": "other_pool", "custom_profiling_tag": "world"},
            authenticated_user="egor",
        )
        wait(lambda: op2.get_state() == "running")
        op3 = run_sleeping_vanilla(
            spec={"pool": "other_pool", "custom_profiling_tag": "hello"},
            authenticated_user="egor",
        )
        wait(lambda: op3.get_state() == "running")
        op4 = run_sleeping_vanilla(
            spec={"pool": "other_pool", "custom_profiling_tag": "hello"},
            authenticated_user="egor",
        )
        wait(lambda: op4.get_state() == "running")

        tags = {"pool": "some_pool", "user_name": "ignat"}
        wait(lambda: are_almost_equal(dominant_fair_share_sensor.get(tags=tags), 0.5))
        wait(lambda: dominant_usage_share_sensor.get(tags=tags) == 1.0)
        wait(lambda: dominant_demand_share_sensor.get(tags=tags) == 1.0)
        wait(lambda: are_almost_equal(dominant_promised_fair_share_sensor.get(tags=tags), 0.5))
        wait(lambda: cpu_usage_sensor.get(tags=tags) == 1)
        wait(lambda: user_slots_usage_sensor.get(tags=tags) == 1)
        wait(lambda: cpu_demand_sensor.get(tags=tags) == 1)
        wait(lambda: user_slots_demand_sensor.get(tags=tags) == 1)

        def get_total_metric_by_users(metric, tags_):
            return sum(metric.get(tags=dict({"user_name": user}, **tags_), default=0) for user in ("ignat", "egor"))

        tags = {"pool": "other_pool", "custom": "hello"}
        wait(lambda: are_almost_equal(get_total_metric_by_users(dominant_fair_share_sensor, tags), 1.0 / 3.0))
        wait(lambda: get_total_metric_by_users(dominant_usage_share_sensor, tags) == 0)
        wait(lambda: get_total_metric_by_users(dominant_demand_share_sensor, tags) == 2.0)
        wait(lambda: are_almost_equal(get_total_metric_by_users(dominant_promised_fair_share_sensor, tags), 1.0 / 3.0))
        wait(lambda: get_total_metric_by_users(cpu_usage_sensor, tags) == 0)
        wait(lambda: get_total_metric_by_users(user_slots_usage_sensor, tags) == 0)
        wait(lambda: get_total_metric_by_users(cpu_demand_sensor, tags) == 2)
        wait(lambda: get_total_metric_by_users(user_slots_demand_sensor, tags) == 2)

        tags = {"pool": "other_pool", "custom": "world"}
        wait(lambda: are_almost_equal(get_total_metric_by_users(dominant_fair_share_sensor, tags), 1.0 / 6.0))
        wait(lambda: get_total_metric_by_users(dominant_usage_share_sensor, tags) == 0)
        wait(lambda: get_total_metric_by_users(dominant_demand_share_sensor, tags) == 1.0)
        wait(lambda: are_almost_equal(get_total_metric_by_users(dominant_promised_fair_share_sensor, tags), 1.0 / 6.0))
        wait(lambda: get_total_metric_by_users(cpu_usage_sensor, tags) == 0)
        wait(lambda: get_total_metric_by_users(user_slots_usage_sensor, tags) == 0)
        wait(lambda: get_total_metric_by_users(cpu_demand_sensor, tags) == 1)
        wait(lambda: get_total_metric_by_users(user_slots_demand_sensor, tags) == 1)

        tags = {"pool": "other_pool", "user_name": "egor"}
        wait(lambda: are_almost_equal(dominant_fair_share_sensor.get(tags=tags), 0.5))
        wait(lambda: dominant_usage_share_sensor.get(tags=tags) == 0)
        wait(lambda: dominant_demand_share_sensor.get(tags=tags) == 3.0)
        wait(lambda: are_almost_equal(dominant_promised_fair_share_sensor.get(tags=tags), 0.5))
        wait(lambda: cpu_usage_sensor.get(tags=tags) == 0)
        wait(lambda: user_slots_usage_sensor.get(tags=tags) == 0)
        wait(lambda: cpu_demand_sensor.get(tags=tags) == 3)
        wait(lambda: user_slots_demand_sensor.get(tags=tags) == 3)

        op4.abort(wait_until_finished=True)
        op3.abort(wait_until_finished=True)
        op1.abort(wait_until_finished=True)

        tags = {"pool": "other_pool", "user_name": "egor"}
        wait(lambda: dominant_fair_share_sensor.get(tags=tags) == 1.0)
        wait(lambda: dominant_usage_share_sensor.get(tags=tags) == 1.0)
        wait(lambda: dominant_demand_share_sensor.get(tags=tags) == 1.0)
        wait(lambda: are_almost_equal(dominant_promised_fair_share_sensor.get(tags=tags), 0.5))

        tags = {"pool": "other_pool", "custom": "world"}
        wait(lambda: get_total_metric_by_users(dominant_fair_share_sensor, tags) == 1.0)
        wait(lambda: get_total_metric_by_users(dominant_usage_share_sensor, tags) == 1.0)
        wait(lambda: get_total_metric_by_users(dominant_demand_share_sensor, tags) == 1.0)
        wait(lambda: are_almost_equal(get_total_metric_by_users(dominant_promised_fair_share_sensor, tags), 0.5))

    @authors("ignat", "eshcherbin")
    def test_job_count_profiling(self):
        self._prepare_tables()

        start_profiling = get_job_count_profiling(self.Env.create_native_client())

        def get_new_jobs_with_state(state):
            current_profiling = get_job_count_profiling(self.Env.create_native_client())
            return current_profiling["state"][state] - start_profiling["state"][state]

        op = map(
            track=False,
            command=with_breakpoint("echo '{foo=bar}'; BREAKPOINT"),
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
        )

        wait(lambda: get_new_jobs_with_state("running") == 1)

        for job in op.get_running_jobs():
            abort_job(job)

        wait(lambda: get_new_jobs_with_state("aborted") == 1)

        release_breakpoint()
        op.track()

        wait(lambda: get_new_jobs_with_state("completed") == 1)

        assert op.get_state() == "completed"


##################################################################


class TestSchedulerProfilingOnOperationFinished(YTEnvSetup, PrepareTables):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "connect_retry_backoff_time": 100,
            "fair_share_update_period": 100,
            "profiling_update_period": 100,
            "fair_share_profiling_period": 100,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operation_time_limit_check_period": 100,
            "operation_controller_fail_timeout": 3000,
            "operation_job_metrics_push_period": 1000000000,
            "job_metrics_report_period": 100,
            "custom_job_metrics": [
                {
                    "statistics_path": "/custom/value_completed",
                    "profiling_name": "metric_completed",
                    "aggregate_type": "sum",
                },
                {
                    "statistics_path": "/custom/value_failed",
                    "profiling_name": "metric_failed",
                    "aggregate_type": "sum",
                },
            ],
        }
    }

    USE_PORTO = True
    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {
                "heartbeat_period": 100,  # 100 msec
            },
        }
    }

    def _get_cypress_metrics(self, operation_id, key, job_state="completed", aggr="sum"):
        statistics = get(get_operation_cypress_path(operation_id) + "/@progress/job_statistics")
        return get_statistics(statistics, "{0}.$.{1}.map.{2}".format(key, job_state, aggr))

    @authors("eshcherbin")
    def test_operation_completed(self):
        create_pool("unique_pool")

        metric_completed_counter = Profiler\
            .at_scheduler(self.Env.create_native_client(), fixed_tags={"tree": "default", "pool": "unique_pool"})\
            .counter("scheduler/pools/metrics/metric_completed")

        cmd = """python -c "import os; os.write(5, '{value_completed=117};')";"""
        run_test_vanilla(cmd, spec={"pool": "unique_pool"})

        wait(lambda: metric_completed_counter.get() == 117)

    @authors("eshcherbin")
    def test_operation_failed(self):
        create_pool("unique_pool")

        metric_failed_counter = Profiler \
            .at_scheduler(self.Env.create_native_client(), fixed_tags={"tree": "default", "pool": "unique_pool"}) \
            .counter("scheduler/pools/metrics/metric_failed")

        cmd = """python -c "import os; os.write(5, '{value_failed=225};')"; exit 1"""
        op = run_test_vanilla(
            command=cmd,
            spec={"max_failed_job_count": 1, "pool": "unique_pool"},
        )
        op.track(raise_on_failed=False)

        wait(lambda: metric_failed_counter.get_delta() == 225)


##################################################################


class TestSchedulerErrorTruncate(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1
    NUM_SECONDARY_MASTER_CELLS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_reporter": {
                "enabled": True,
                "reporting_period": 10,
                "min_repeat_delay": 10,
                "max_repeat_delay": 10,
            },
            "test_job_error_truncation": True,
        },
    }

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "enable_job_reporter": True,
            "enable_job_spec_reporter": True,
            "enable_job_stderr_reporter": True,
        }
    }

    @classmethod
    def modify_node_config(cls, config):
        config["cluster_connection"]["primary_master"]["rpc_timeout"] = 50000
        for connection in config["cluster_connection"]["secondary_masters"]:
            connection["rpc_timeout"] = 50000

    def setup(self):
        sync_create_cells(1)
        init_operation_archive.create_tables_latest_version(
            self.Env.create_native_client(), override_tablet_cell_bundle="default"
        )
        self._tmpdir = create_tmpdir("jobids")

    def teardown(self):
        remove("//sys/operations_archive")

    @authors("ignat")
    def test_error_truncate(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")
        write_table("//tmp/t_in", {"foo": "bar"})

        op = map(
            command=with_breakpoint("BREAKPOINT; echo '{foo=bar}'; exit 1"),
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            spec={"max_failed_job_count": 1},
            track=False,
        )

        wait(lambda: op.get_running_jobs())
        running_job = op.get_running_jobs().keys()[0]

        release_breakpoint()
        op.track(raise_on_failed=False)

        def find_truncated_errors(error):
            assert len(error.get("inner_errors", [])) <= 2
            if error.get("attributes", {}).get("inner_errors_truncated", False):
                return True
            return any([find_truncated_errors(inner_error) for inner_error in error.get("inner_errors", [])])

        job_error = get_job(job_id=running_job, operation_id=op.id)["error"]
        assert find_truncated_errors(job_error)


##################################################################


class TestSafeAssertionsMode(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "enable_controller_failure_spec_option": True,
        },
    }

    VERY_USEFUL_AND_READABLE_LOGS = [
        "coredumper.log",
        "coredumper_inner.log",
        "coredumper_lister.log",
    ]

    @classmethod
    def setup_class(cls):
        super(TestSafeAssertionsMode, cls).setup_class()

        for log in cls.VERY_USEFUL_AND_READABLE_LOGS:
            open("/tmp/" + log, "w")

    @classmethod
    def teardown_class(cls):
        shutil.rmtree(cls.core_path)

        for log in cls.VERY_USEFUL_AND_READABLE_LOGS:
            if os.path.exists("/tmp/" + log):
                shutil.copyfile("/tmp/" + log, cls.path_to_run + "/" + log)
        super(TestSafeAssertionsMode, cls).teardown_class()

    @classmethod
    def modify_controller_agent_config(cls, config):
        cls.core_path = os.path.join(cls.path_to_run, "_cores")
        os.mkdir(cls.core_path)
        os.chmod(cls.core_path, 0777)
        config["core_dumper"] = {
            "path": cls.core_path,
            # Pattern starts with the underscore to trick teamcity; we do not want it to
            # pay attention to the created core.
            "pattern": "_core.%CORE_PID.%CORE_SIG.%CORE_THREAD_NAME-%CORE_REASON",
        }

    @authors("max42")
    @pytest.mark.skipif(is_asan_build(), reason="Core dumps + ASAN = no way")
    def test_assertion_failure(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})
        create("table", "//tmp/t_out")

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"testing": {"controller_failure": "assertion_failure_in_prepare"}},
            command="cat",
        )
        with pytest.raises(YtError):
            op.track()

        err = op.get_error()
        print_debug("=== error ===")
        print_debug(err)

        assert err.contains_code(212)  # NScheduler::EErrorCode::OperationControllerCrashed

        # Core path is either attribute of an error itself, or of the only inner error when it is
        # wrapped with 'Operation has failed to prepare' error.
        core_path = err.attributes.get("core_path") or err.inner_errors[0].get("attributes", {}).get("core_path")
        assert core_path != YsonEntity()

        # Wait until core is finished. This may take a really long time under debug :(
        controller_agent_address = get(op.get_path() + "/@controller_agent_address")

        def check_core():
            if not os.path.exists(core_path):
                print_debug("size = n/a")
            else:
                print_debug("size =", os.stat(core_path).st_size)
            return (
                get(
                    "//sys/controller_agents/instances/{}/orchid/core_dumper/active_count".format(
                        controller_agent_address
                    )
                )
                == 0
            )

        wait(check_core, iter=200, sleep_backoff=5)

        gdb = arcadia_interop.yatest_common.gdb_path()

        assert os.path.exists(core_path)
        child = subprocess.Popen(
            [
                gdb,
                "--batch",
                "-ex",
                "bt",
                os.path.join(self.bin_path, "ytserver-controller-agent"),
                core_path,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        stdout, stderr = child.communicate()
        print_debug("=== stderr ===")
        print_debug(stderr)
        print_debug("=== stdout ===")
        print_debug(stdout)
        assert child.returncode == 0
        assert "Prepare" in stdout

    @authors("ignat")
    def test_unexpected_exception(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})
        create("table", "//tmp/t_out")

        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"testing": {"controller_failure": "exception_thrown_in_on_job_completed"}},
            command="cat",
        )
        with pytest.raises(YtError):
            op.track()
        print_debug(op.get_error())
        assert op.get_error().contains_code(213)  # NScheduler::EErrorCode::TestingError


##################################################################


class TestAsyncControllerActions(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 2  # snapshot upload replication factor is 2; unable to configure
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "heavy_request_immediate_response_timeout": 250,
        },
    }

    # NB(eshcherbin): These first two tests are probably meaningless, but let them be.
    @authors("eshcherbin")
    def test_fast_operation_flow(self):
        op = run_test_vanilla("sleep 1")
        op.track()

    @authors("eshcherbin")
    def test_fast_operation_flow_revive(self):
        op = run_test_vanilla(with_breakpoint("BREAKPOINT"))

        wait_breakpoint()

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        release_breakpoint()

        op.track()

    @authors("eshcherbin")
    def test_slow_initialization(self):
        op = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_initialize": 500}})
        op.track()

    @authors("eshcherbin")
    def test_slow_initialization_two_operations(self):
        op1 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_initialize": 600}})
        op2 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_initialize": 500}})
        op1.track()
        op2.track()

    @authors("eshcherbin")
    def test_slow_initialization_revive(self):
        op = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_initialize": 1500}})
        op.wait_for_state("initializing")

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        op.track()

    @authors("eshcherbin")
    def test_slow_preparation(self):
        op = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_prepare": 500}})
        op.track()

    @authors("eshcherbin")
    def test_slow_preparation_two_operations(self):
        op1 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_prepare": 600}})
        op2 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_prepare": 500}})
        op1.track()
        op2.track()

    @authors("eshcherbin")
    def test_slow_materialization(self):
        op = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_materialize": 500}})
        op.track()

    @authors("eshcherbin")
    def test_slow_materialization_two_operations(self):
        op1 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_materialize": 600}})
        op2 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_materialize": 500}})
        op1.track()
        op2.track()

    @authors("eshcherbin")
    def test_slow_materialization_revive(self):
        op = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_materialize": 1500}})
        op.wait_for_state("materializing")

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        op.track()

    @authors("eshcherbin")
    def test_slow_revival_from_scratch(self):
        # Use "delay_inside_prepare" here because revival from scratch is basically preparation.
        op = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_prepare": 500}})

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        op.track()

    @authors("eshcherbin")
    def test_slow_revival_from_snapshot(self):
        update_controller_agent_config("snapshot_period", 300)

        op = run_test_vanilla(
            with_breakpoint("BREAKPOINT"),
            spec={"testing": {"delay_inside_revive": 500}},
        )

        op.wait_for_fresh_snapshot()

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        release_breakpoint()

        op.track()

    @authors("eshcherbin")
    def test_slow_revival_from_scratch_two_operations(self):
        # Use "delay_inside_prepare" here because revival from scratch is basically preparation.
        op1 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_prepare": 500}})
        op2 = run_test_vanilla("sleep 1", spec={"testing": {"delay_inside_prepare": 500}})

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        op1.track()
        op2.track()

    @authors("eshcherbin")
    def test_slow_revival_from_snapshot_two_operations(self):
        update_controller_agent_config("snapshot_period", 300)

        op1 = run_test_vanilla(
            with_breakpoint("BREAKPOINT"),
            spec={"testing": {"delay_inside_revive": 500}},
        )
        op2 = run_test_vanilla(
            with_breakpoint("BREAKPOINT"),
            spec={"testing": {"delay_inside_revive": 500}},
        )

        op1.wait_for_fresh_snapshot()
        op2.wait_for_fresh_snapshot()

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        release_breakpoint()

        op1.track()
        op2.track()

    @authors("eshcherbin")
    def test_slow_commit(self):
        op = run_test_vanilla(
            "sleep 1",
            spec={
                "testing": {
                    "delay_inside_operation_commit": 500,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )
        op.track()

    @authors("eshcherbin")
    def test_slow_commit_two_operations(self):
        op1 = run_test_vanilla(
            "sleep 1",
            spec={
                "testing": {
                    "delay_inside_operation_commit": 600,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )
        op2 = run_test_vanilla(
            "sleep 1",
            spec={
                "testing": {
                    "delay_inside_operation_commit": 500,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )
        op1.track()
        op2.track()

    @authors("eshcherbin")
    def test_slow_commit_revive(self):
        op = run_test_vanilla(
            "sleep 1",
            spec={
                "testing": {
                    "delay_inside_operation_commit": 1500,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )
        op.wait_for_state("completing")

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        op.track()

    @authors("eshcherbin")
    def test_slow_everything(self):
        op = run_test_vanilla(
            "sleep 1",
            spec={
                "testing": {
                    "delay_inside_initialize": 500,
                    "delay_inside_prepare": 500,
                    "delay_inside_materialize": 500,
                    "delay_inside_operation_commit": 500,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )
        op.track()

    @authors("eshcherbin")
    def test_slow_everything_two_operations(self):
        op1 = run_test_vanilla(
            "sleep 1",
            spec={
                "testing": {
                    "delay_inside_initialize": 600,
                    "delay_inside_prepare": 600,
                    "delay_inside_materialize": 600,
                    "delay_inside_operation_commit": 600,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )
        op2 = run_test_vanilla(
            "sleep 1",
            spec={
                "testing": {
                    "delay_inside_initialize": 500,
                    "delay_inside_prepare": 500,
                    "delay_inside_materialize": 500,
                    "delay_inside_operation_commit": 500,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )
        op1.track()
        op2.track()

    @authors("eshcherbin")
    def test_slow_everything_revive(self):
        op = run_test_vanilla(
            with_breakpoint("BREAKPOINT"),
            spec={
                "testing": {
                    "delay_inside_initialize": 500,
                    "delay_inside_prepare": 500,
                    "delay_inside_materialize": 500,
                    "delay_inside_operation_commit": 500,
                    "delay_inside_operation_commit_stage": "start",
                }
            },
        )

        with Restarter(self.Env, CONTROLLER_AGENTS_SERVICE):
            pass

        release_breakpoint()

        op.track()


class TestControllerAgentPrerequisiteTxError(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 2  # snapshot upload replication factor is 2; unable to configure
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "controller_agent_tracker": {
                "incarnation_transaction_ping_period": 10000,
                "incarnation_transaction_timeout": 30000,
            }
        }
    }
    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10000,
        },
    }

    def _abort_controller_agent_incarnation_transaction(self):
        incarnation_tx = None
        for tx in ls("//sys/transactions", attributes=["title"]):
            title = tx.attributes.get("title", "")
            id = str(tx)
            if "Controller agent incarnation" in title:
                incarnation_tx = id
        assert incarnation_tx is not None

        abort_transaction(incarnation_tx)

    @authors("ignat")
    def test_incarnation_transaction_abort(self):
        create("table", "//tmp/test_input")
        create("table", "//tmp/test_output")
        write_table("//tmp/test_input", [{"a": "b"}])
        op = map(
            track=False,
            command="sleep 1",
            in_="//tmp/test_input",
            out="//tmp/test_output",
            spec={"testing": {"delay_inside_initialize": 5000}},
        )
        # This sleep is intentional, it is usually enough to enter initialization, but test still should be correct if it does not happen.
        time.sleep(3)
        self._abort_controller_agent_incarnation_transaction()
        op.track()


class TestControllerAgentDisconnectionDuringUnregistration(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 2  # snapshot upload replication factor is 2; unable to configure
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "controller_agent_tracker": {
                "heavy_rpc_timeout": 12000,
                "light_rpc_timeout": 8000,
            }
        }
    }
    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "testing_options": {
                "delay_in_unregistration": 20000,
            }
        },
    }

    @authors("ignat")
    def test_no_revive_after_unregistration(self):
        create("table", "//tmp/test_input")
        create("table", "//tmp/test_output")
        write_table("//tmp/test_input", [{"a": "b"}])

        transaction_id = start_transaction(timeout=300 * 1000)
        op = map(
            track=False,
            command="sleep 1000",
            in_="//tmp/test_input",
            out="//tmp/test_output",
            transaction_id=transaction_id,
        )

        wait(lambda: op.get_running_jobs())

        # TODO(ignat): avoid this sleep.
        time.sleep(2)

        abandon_job(op.get_running_jobs().keys()[0])

        wait(lambda: op.get_state() == "completed")

        # Wait for delay in unregistration.
        time.sleep(15)

        assert op.get_state() == "completed"
