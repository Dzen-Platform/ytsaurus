import pytest

from yt_commands import *
from yt_helpers import *

from yt_env_setup import (
    YTEnvSetup,
    wait,
    Restarter,
    SCHEDULERS_SERVICE,
    CONTROLLER_AGENTS_SERVICE,
    is_asan_build,
)
from yt.test_helpers import are_almost_equal

from flaky import flaky

import os
import time
import datetime

import __builtin__

##################################################################


def get_scheduling_options(user_slots):
    return {"scheduling_options_per_pool_tree": {"default": {"resource_limits": {"user_slots": user_slots}}}}


def get_from_tree_orchid(tree, path, **kwargs):
    return get("//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/{}/{}".format(tree, path), **kwargs)


##################################################################


class TestResourceUsage(YTEnvSetup, PrepareTables):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "connect_retry_backoff_time": 100,
            "fair_share_update_period": 100,
            "fair_share_profiling_period": 100,
            "alerts_update_period": 100,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operation_time_limit_check_period": 100,
        }
    }

    DELTA_NODE_CONFIG = {
        "resource_limits": {
            "user_jobs": {
                "type": "static",
                "value": 10 ** 9,
            },
        }
    }

    def setup_method(self, method):
        super(TestResourceUsage, self).setup_method(method)
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 0)
        time.sleep(0.5)

    def _check_running_jobs(self, op, desired_running_jobs):
        success_iter = 0
        min_success_iteration = 10
        for i in xrange(100):
            running_jobs = op.get_running_jobs()
            if running_jobs:
                assert len(running_jobs) <= desired_running_jobs
                success_iter += 1
                if success_iter == min_success_iteration:
                    return
            time.sleep(0.1)
        assert False

    @authors("ignat")
    def test_root_pool(self):
        wait(
            lambda: are_almost_equal(
                get(scheduler_orchid_default_pool_tree_path() + "/pools/<Root>/fair_share_ratio"),
                0.0,
            )
        )

    @authors("ignat")
    def test_scheduler_unlimited_demand_fair_share_ratio(self):
        total_resource_limits = get("//sys/scheduler/orchid/scheduler/cell/resource_limits")

        create_pool(
            "big_pool",
            attributes={"min_share_resources": {"cpu": total_resource_limits["cpu"]}},
        )
        create_pool("subpool_1", parent_name="big_pool", attributes={"weight": 1.0})
        create_pool("subpool_2", parent_name="big_pool", attributes={"weight": 3.0})
        create_pool("small_pool", attributes={"weight": 100.0})
        create_pool("subpool_3", parent_name="small_pool")
        create_pool("subpool_4", parent_name="small_pool")

        # Wait for fair share update.
        time.sleep(1)

        get_pool_unlimited_demand_fair_share_resources = lambda pool: get(
            scheduler_orchid_pool_path(pool) + "/unlimited_demand_fair_share_resources"
        )

        get_pool_unlimited_demand_fair_share_ratio = lambda pool: get(
            scheduler_orchid_pool_path(pool) + "/unlimited_demand_fair_share_ratio"
        )

        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("big_pool"), 1.0)
        assert get_pool_unlimited_demand_fair_share_resources("big_pool") == total_resource_limits

        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("small_pool"), 0)
        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("subpool_3"), 0)
        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("subpool_4"), 0)

        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("subpool_1"), 1.0 / 4.0)
        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("subpool_2"), 3.0 / 4.0)

        self._prepare_tables()

        get_operation_unlimited_demand_fair_share_ratio = lambda op_id: get(
            "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/unlimited_demand_fair_share_ratio".format(
                op_id
            )
        )

        op = map(
            track=False,
            command=with_breakpoint("cat ; BREAKPOINT"),
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            spec={"pool": "big_pool"},
        )
        wait_breakpoint()

        # Wait for fair share update.
        time.sleep(1)

        assert are_almost_equal(get_operation_unlimited_demand_fair_share_ratio(op.id), 1.0 / 5.0)
        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("subpool_1"), 1.0 / 5.0)
        assert are_almost_equal(get_pool_unlimited_demand_fair_share_ratio("subpool_2"), 3.0 / 5.0)

        release_breakpoint()
        op.track()

    @authors("ignat")
    def test_resource_limits(self):
        resource_limits = {"cpu": 1, "memory": 1000 * 1024 * 1024, "network": 10}
        create_pool("test_pool", attributes={"resource_limits": resource_limits})

        # TODO(renadeen): Do better, I know you can.
        def check_limits():
            stats = get(scheduler_orchid_default_pool_tree_path())
            pool_resource_limits = stats["pools"]["test_pool"]["resource_limits"]
            for resource, limit in resource_limits.iteritems():
                resource_name = "user_memory" if resource == "memory" else resource
                if not are_almost_equal(pool_resource_limits[resource_name], limit):
                    return False
            return True

        wait(check_limits)

        self._prepare_tables()
        data = [{"foo": i} for i in xrange(3)]
        write_table("//tmp/t_in", data)

        memory_limit = 30 * 1024 * 1024

        testing_options = {"scheduling_delay": 500, "scheduling_delay_type": "async"}

        op = map(
            track=False,
            command="sleep 100",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "job_count": 3,
                "pool": "test_pool",
                "mapper": {"memory_limit": memory_limit},
                "testing": testing_options,
            },
        )
        self._check_running_jobs(op, 1)
        op.abort()

        op = map(
            track=False,
            command="sleep 5",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "job_count": 3,
                "resource_limits": resource_limits,
                "mapper": {"memory_limit": memory_limit},
                "testing": testing_options,
            },
        )
        self._check_running_jobs(op, 1)
        op_limits = get(
            "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/resource_limits".format(
                op.id
            )
        )
        for resource, limit in resource_limits.iteritems():
            resource_name = "user_memory" if resource == "memory" else resource
            assert are_almost_equal(op_limits[resource_name], limit)

    @authors("ignat")
    def test_resource_limits_preemption(self):
        create_pool("test_pool2")

        self._prepare_tables()
        data = [{"foo": i} for i in xrange(3)]
        write_table("//tmp/t_in", data)

        op = map(
            track=False,
            command="sleep 100",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"job_count": 3, "pool": "test_pool2"},
        )
        wait(lambda: len(op.get_running_jobs()) == 3)

        resource_limits = {"cpu": 2}
        set("//sys/pools/test_pool2/@resource_limits", resource_limits)

        wait(
            lambda: are_almost_equal(
                get(scheduler_orchid_default_pool_tree_path() + "/pools/test_pool2/resource_limits/cpu"),
                2,
            )
        )

        wait(lambda: len(op.get_running_jobs()) == 2)

    # Remove flaky after YT-8784.
    @authors("ignat")
    @flaky(max_runs=5)
    def test_resource_limits_runtime(self):
        self._prepare_tables()
        data = [{"foo": i} for i in xrange(3)]
        write_table("//tmp/t_in", data)

        op = map(
            track=False,
            command="sleep 100",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"job_count": 3, "resource_limits": {"user_slots": 1}},
        )
        self._check_running_jobs(op, 1)

        set(op.get_path() + "/@resource_limits", {"user_slots": 2})
        self._check_running_jobs(op, 2)

    @authors("ignat")
    def test_max_possible_resource_usage(self):
        create_pool("low_cpu_pool", attributes={"resource_limits": {"cpu": 1}})
        create_pool("subpool_1", parent_name="low_cpu_pool")
        create_pool(
            "subpool_2",
            parent_name="low_cpu_pool",
            attributes={"resource_limits": {"cpu": 0}},
        )
        create_pool("high_cpu_pool")

        self._create_table("//tmp/t_in")
        self._create_table("//tmp/t_out_1")
        self._create_table("//tmp/t_out_2")
        self._create_table("//tmp/t_out_3")
        data = [{"foo": i} for i in xrange(3)]
        write_table("//tmp/t_in", data)

        def get_pool_fair_share(pool, resource):
            return get("{0}/pools/{1}/detailed_fair_share/total/{2}".format(scheduler_orchid_default_pool_tree_path(), pool, resource))

        command_with_breakpoint = with_breakpoint("cat ; BREAKPOINT")
        op1 = map(
            track=False,
            command=command_with_breakpoint,
            in_="//tmp/t_in",
            out="//tmp/t_out_1",
            spec={"job_count": 1, "pool": "subpool_1"},
        )

        op2 = map(
            track=False,
            command=command_with_breakpoint,
            in_="//tmp/t_in",
            out="//tmp/t_out_2",
            spec={"job_count": 2, "pool": "high_cpu_pool"},
        )

        wait_breakpoint()

        for resource in ["cpu", "user_slots"]:
            wait(lambda: are_almost_equal(get_pool_fair_share("subpool_1", resource), 1.0 / 3.0))
            wait(lambda: are_almost_equal(get_pool_fair_share("subpool_2", resource), 0.0))
            wait(lambda: are_almost_equal(get_pool_fair_share("low_cpu_pool", resource), 1.0 / 3.0))
            wait(lambda: are_almost_equal(get_pool_fair_share("high_cpu_pool", resource), 2.0 / 3.0))

        op3 = map(
            track=False,
            command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out_3",
            spec={"job_count": 1, "pool": "subpool_2", "mapper": {"cpu_limit": 0}},
        )

        time.sleep(1)

        wait(lambda: are_almost_equal(get_pool_fair_share("subpool_1", "user_slots"), 1.0 / 4.0))
        wait(lambda: are_almost_equal(get_pool_fair_share("subpool_2", "user_slots"), 1.0 / 4.0))
        wait(lambda: are_almost_equal(get_pool_fair_share("low_cpu_pool", "user_slots"), 1.0 / 2.0))
        wait(lambda: are_almost_equal(get_pool_fair_share("high_cpu_pool", "user_slots"), 1.0 / 2.0))

        wait(lambda: are_almost_equal(get_pool_fair_share("subpool_1", "cpu"), 1.0 / 4.0))
        wait(lambda: are_almost_equal(get_pool_fair_share("subpool_2", "cpu"), 0.0))
        wait(lambda: are_almost_equal(get_pool_fair_share("low_cpu_pool", "cpu"), 1.0 / 4.0))
        wait(lambda: are_almost_equal(get_pool_fair_share("high_cpu_pool", "cpu"), 1.0 / 2.0))

        release_breakpoint()
        op1.track()
        op2.track()
        op3.track()

    @authors("renadeen", "ignat")
    def test_fractional_cpu_usage(self):
        self._create_table("//tmp/t_in")
        self._create_table("//tmp/t_out")
        data = [{"foo": i} for i in xrange(3)]
        write_table("//tmp/t_in", data)

        op = map(
            track=False,
            command=with_breakpoint("cat ; BREAKPOINT"),
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"job_count": 3, "mapper": {"cpu_limit": 0.87}},
        )
        wait_breakpoint()

        get_resource_usage = lambda op: get(
            "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/resource_usage".format(
                op.id
            )
        )
        wait(lambda: are_almost_equal(get_resource_usage(op)["cpu"], 3 * 0.87))

        release_breakpoint()
        op.track()


##################################################################


class TestStrategyWithSlowController(YTEnvSetup, PrepareTables):
    NUM_MASTERS = 1
    NUM_NODES = 10
    NUM_SCHEDULERS = 1

    CONCURRENT_HEARTBEAT_LIMIT = 2

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "node_shard_count": 1,
            "soft_concurrent_heartbeat_limit": CONCURRENT_HEARTBEAT_LIMIT,
            "hard_concurrent_heartbeat_limit": CONCURRENT_HEARTBEAT_LIMIT,
        }
    }

    DELTA_NODE_CONFIG = {"exec_agent": {"scheduler_connector": {"heartbeat_period": 100}}}

    @authors("renadeen", "ignat")
    def test_strategy_with_slow_controller(self):
        slow_spec = {"testing": {"scheduling_delay": 1000, "scheduling_delay_type": "async"}}

        # Occupy the cluster
        op0 = run_test_vanilla(with_breakpoint("BREAKPOINT"), job_count=10)
        wait_breakpoint(job_count=10)

        # Run operations
        op1 = run_test_vanilla(with_breakpoint("BREAKPOINT"), job_count=20)
        op2 = run_test_vanilla(with_breakpoint("BREAKPOINT"), job_count=20, spec=slow_spec)

        wait(lambda: op1.get_state() == "running")
        wait(lambda: op2.get_state() == "running")

        enable_op_detailed_logs(op1)
        enable_op_detailed_logs(op2)

        assert op1.get_job_count("running") == 0
        assert op2.get_job_count("running") == 0

        # Free up the cluster
        for j in op0.get_running_jobs():
            release_breakpoint(job_id=j)

        # Check the resulting allocation
        wait(lambda: op1.get_job_count("running") + op2.get_job_count("running") == 10)
        assert abs(op1.get_job_count("running") - op2.get_job_count("running")) <= self.CONCURRENT_HEARTBEAT_LIMIT


# TODO(ignat): Rename this suite as its current name is too generic.
class TestStrategies(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 2
    NUM_SCHEDULERS = 1

    def _prepare_tables(self):
        create("table", "//tmp/t_in")
        set("//tmp/t_in/@replication_factor", 1)
        write_table("//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out")
        set("//tmp/t_out/@replication_factor", 1)

    def _get_table_chunk_node(self, table):
        chunk_id = get_first_chunk_id(table)
        replicas = get("#{0}/@stored_replicas".format(chunk_id))
        assert len(replicas) == 1

        return replicas[0]

    @authors("ignat")
    def test_strategies(self):
        self._prepare_tables()

        node = self._get_table_chunk_node("//tmp/t_in")
        set_banned_flag(True, [node])

        print_debug("Fail strategy")
        with pytest.raises(YtError):
            map(
                in_="//tmp/t_in",
                out="//tmp/t_out",
                command="cat",
                spec={"unavailable_chunk_strategy": "fail"},
            )

        print_debug("Skip strategy")
        map(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat",
            spec={"unavailable_chunk_strategy": "skip"},
        )
        assert read_table("//tmp/t_out") == []

        print_debug("Wait strategy")
        op = map(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            command="cat",
            spec={"unavailable_chunk_strategy": "wait"},
        )

        set_banned_flag(False, [node])
        op.track()

        assert read_table("//tmp/t_out") == [{"foo": "bar"}]

    @authors("ignat")
    def test_strategies_in_sort(self):
        v1 = {"key": "aaa"}
        v2 = {"key": "bb"}
        v3 = {"key": "bbxx"}
        v4 = {"key": "zfoo"}
        v5 = {"key": "zzz"}

        create("table", "//tmp/t_in")
        set("//tmp/t_in/@replication_factor", 1)
        write_table("//tmp/t_in", [v3, v5, v1, v2, v4])  # some random order

        create("table", "//tmp/t_out")
        set("//tmp/t_out/@replication_factor", 1)

        set_banned_flag(True)

        print_debug("Fail strategy")
        with pytest.raises(YtError):
            sort(
                in_="//tmp/t_in",
                out="//tmp/t_out",
                sort_by="key",
                spec={"unavailable_chunk_strategy": "fail"},
            )

        print_debug("Skip strategy")
        sort(
            in_="//tmp/t_in",
            out="//tmp/t_out",
            sort_by="key",
            spec={"unavailable_chunk_strategy": "skip"},
        )
        assert read_table("//tmp/t_out") == []

        print_debug("Wait strategy")
        op = sort(
            track=False,
            in_="//tmp/t_in",
            out="//tmp/t_out",
            sort_by="key",
            spec={"unavailable_chunk_strategy": "wait"},
        )

        # Give a chance to scraper to work
        time.sleep(1.0)
        set_banned_flag(False)
        op.track()

        assert read_table("//tmp/t_out") == [v1, v2, v3, v4, v5]
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["key"]

    @authors("ignat")
    def test_strategies_in_merge(self):
        create("table", "//tmp/t1")
        set("//tmp/t1/@replication_factor", 1)
        write_table("<append=true>//tmp/t1", [{"a": 0}, {"a": 2}], sorted_by="a")
        write_table("<append=true>//tmp/t1", [{"a": 4}, {"a": 6}], sorted_by="a")

        create("table", "//tmp/t2")
        set("//tmp/t2/@replication_factor", 1)
        write_table("<append=true>//tmp/t2", [{"a": 1}, {"a": 3}], sorted_by="a")
        write_table("<append=true>//tmp/t2", [{"a": 5}, {"a": 7}], sorted_by="a")

        create("table", "//tmp/t_out")
        set("//tmp/t_out/@replication_factor", 1)

        set_banned_flag(True)

        print_debug("Fail strategy")
        with pytest.raises(YtError):
            merge(
                mode="sorted",
                in_=["//tmp/t1", "//tmp/t2"],
                out="//tmp/t_out",
                spec={"unavailable_chunk_strategy": "fail"},
            )

        print_debug("Skip strategy")
        merge(
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2"],
            out="//tmp/t_out",
            spec={"unavailable_chunk_strategy": "skip"},
        )
        assert read_table("//tmp/t_out") == []

        print_debug("Wait strategy")
        op = merge(
            track=False,
            mode="sorted",
            in_=["//tmp/t1", "//tmp/t2"],
            out="//tmp/t_out",
            spec={"unavailable_chunk_strategy": "wait"},
        )

        # Give a chance for scraper to work
        time.sleep(1.0)
        set_banned_flag(False)
        op.track()

        assert read_table("//tmp/t_out") == [{"a": i} for i in range(8)]
        assert get("//tmp/t_out/@sorted")
        assert get("//tmp/t_out/@sorted_by") == ["a"]


##################################################################


class TestSchedulerOperationLimits(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {"scheduler": {"static_orchid_cache_update_period": 100}}

    def setup_method(self, method):
        super(TestSchedulerOperationLimits, self).setup_method(method)
        set("//sys/pool_trees/default/@config/max_running_operation_count_per_pool", 1)
        set("//sys/pool_trees/default/@config/default_parent_pool", "default_pool")

    def _run_operations(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out1")
        create("table", "//tmp/out2")
        create("table", "//tmp/out3")
        write_table("//tmp/in", [{"foo": "bar"}])

        command = with_breakpoint("cat > /dev/null && BREAKPOINT")
        op1 = map(
            track=False,
            command=command,
            in_=["//tmp/in"],
            out="//tmp/out1",
            spec={"pool": "test_pool_1"},
        )

        op2 = map(
            track=False,
            command=command,
            in_=["//tmp/in"],
            out="//tmp/out2",
            spec={"pool": "test_pool_1"},
        )

        op3 = map(
            track=False,
            command=command,
            in_=["//tmp/in"],
            out="//tmp/out3",
            spec={"pool": "test_pool_2"},
        )

        wait_breakpoint(job_count=2)

        # We sleep some time to make sure that op2 will not start.
        time.sleep(1)

        assert op1.get_state() == "running"
        assert op2.get_state() == "pending"
        assert op3.get_state() == "running"

        release_breakpoint()

        op1.track()
        op2.track()
        op3.track()

        assert read_table("//tmp/out1") == []
        assert read_table("//tmp/out2") == []
        assert read_table("//tmp/out3") == []

    @authors("ignat")
    def test_operations_pool_limit(self):
        create_pool("test_pool_1")
        create_pool("test_pool_2")
        self._run_operations()

    @authors("ignat")
    def test_operations_recursive_pool_limit(self):
        create_pool("research")
        set("//sys/pools/research/@max_running_operation_count", 2)
        create_pool("test_pool_1", parent_name="research")
        create_pool("test_pool_2", parent_name="research")
        self._run_operations()

    @authors("asaitgalin")
    def test_operation_count(self):
        create("table", "//tmp/in")
        write_table("//tmp/in", [{"foo": i} for i in xrange(5)])

        attrs = {"max_running_operation_count": 3}
        create_pool("research", attributes=attrs)
        create_pool("subpool", parent_name="research", attributes=attrs)
        create_pool("other_subpool", parent_name="subpool", attributes=attrs)

        ops = []
        for i in xrange(3):
            create("table", "//tmp/out_" + str(i))
            op = map(
                command="sleep 1000; cat >/dev/null",
                in_=["//tmp/in"],
                out="//tmp/out_" + str(i),
                spec={"pool": "other_subpool"},
                track=False,
            )
            ops.append(op)

        wait(lambda: get(scheduler_orchid_default_pool_tree_path() + "/pools/research/operation_count") == 3)
        wait(lambda: get(scheduler_orchid_default_pool_tree_path() + "/pools/research/running_operation_count") == 3)

    @authors("ignat")
    def test_pending_operations_after_revive(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out1")
        create("table", "//tmp/out2")
        data = [{"foo": i} for i in xrange(5)]
        write_table("//tmp/in", data)

        op1 = map(track=False, command="sleep 5.0; cat", in_=["//tmp/in"], out="//tmp/out1")
        op2 = map(track=False, command="cat", in_=["//tmp/in"], out="//tmp/out2")

        time.sleep(1.5)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        op1.track()
        op2.track()

        assert sorted(read_table("//tmp/out1")) == sorted(data)
        assert sorted(read_table("//tmp/out2")) == sorted(data)

    @authors("ermolovd", "ignat")
    def test_abort_of_pending_operation(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out1")
        create("table", "//tmp/out2")
        create("table", "//tmp/out3")
        write_table("//tmp/in", [{"foo": i} for i in xrange(5)])

        command = with_breakpoint("cat > /dev/null ; BREAKPOINT")
        op1 = map(track=False, command=command, in_=["//tmp/in"], out="//tmp/out1")
        op2 = map(track=False, command=command, in_=["//tmp/in"], out="//tmp/out2")
        op3 = map(track=False, command=command, in_=["//tmp/in"], out="//tmp/out3")

        wait_breakpoint()

        # Sleep some time to make sure that op2 and op3 will not start.
        time.sleep(1)
        assert op1.get_state() == "running"
        assert op2.get_state() == "pending"
        assert op3.get_state() == "pending"

        op2.abort()
        release_breakpoint()
        op1.track()
        op3.track()

        assert op1.get_state() == "completed"
        assert op2.get_state() == "aborted"
        assert op3.get_state() == "completed"

    @authors("ignat")
    def test_reconfigured_pools_operations_limit(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out1")
        create("table", "//tmp/out2")
        write_table("//tmp/in", [{"foo": i} for i in xrange(5)])

        create_pool("test_pool_1")
        create_pool("test_pool_2")

        op1 = map(
            track=False,
            command=with_breakpoint("cat ; BREAKPOINT"),
            in_=["//tmp/in"],
            out="//tmp/out1",
            spec={"pool": "test_pool_1"},
        )
        wait_breakpoint()

        # TODO(ignat): Stabilize this part.
        remove("//sys/pools/test_pool_1")
        create_pool("test_pool_1", parent_name="test_pool_2", wait_for_orchid=False)
        time.sleep(0.5)

        op2 = map(
            track=False,
            command="cat",
            in_=["//tmp/in"],
            out="//tmp/out2",
            spec={"pool": "test_pool_2"},
        )

        assert op1.get_state() == "running"
        wait(lambda: op2.get_state() == "pending")

        release_breakpoint()
        op1.track()
        op2.track()

    @authors("ignat")
    def test_total_operations_limit(self):
        create_pool("research")
        create_pool("research_subpool", parent_name="research")
        create_pool("production")
        set("//sys/pools/research/@max_operation_count", 3)

        create("table", "//tmp/in")
        write_table("//tmp/in", [{"foo": "bar"}])
        for i in xrange(5):
            create("table", "//tmp/out" + str(i))

        ops = []

        def run(index, pool, should_raise):
            def execute(track):
                return map(
                    track=track,
                    command="sleep 1000; cat",
                    in_=["//tmp/in"],
                    out="//tmp/out" + str(index),
                    spec={"pool": pool},
                )

            if should_raise:
                with pytest.raises(YtError):
                    execute(track=True)
            else:
                op = execute(track=False)
                wait(lambda: op.get_state() in ("pending", "running"))
                ops.append(op)

        for i in xrange(3):
            run(i, "research", False)

        for i in xrange(3, 5):
            run(i, "research", True)

        for i in xrange(3, 5):
            run(i, "research_subpool", True)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        for i in xrange(3, 5):
            run(i, "research", True)

        for i in xrange(3, 5):
            run(i, "production", False)

        pools_path = "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/fair_share_info/pools"
        wait(lambda: get(pools_path + "/production/running_operation_count") == 1)
        wait(lambda: get(pools_path + "/production/operation_count") == 2)
        wait(lambda: get(pools_path + "/research/running_operation_count") == 1)
        wait(lambda: get(pools_path + "/research/operation_count") == 3)
        wait(lambda: get(pools_path + "/<Root>/running_operation_count") == 2)
        wait(lambda: get(pools_path + "/<Root>/operation_count") == 5)

        for op in ops:
            op.abort()

    @authors("ignat")
    def test_pool_changes(self):
        create_pool("research")
        create_pool("subpool", parent_name="research")
        create_pool("production")

        create("table", "//tmp/in")
        write_table("//tmp/in", [{"foo": "bar"}])
        for i in xrange(5):
            create("table", "//tmp/out" + str(i))

        ops = []

        def run(index, pool):
            ops.append(
                map(
                    track=False,
                    command="sleep 1000; cat",
                    in_=["//tmp/in"],
                    out="//tmp/out" + str(index),
                    spec={"pool": pool},
                )
            )

        for i in xrange(1, 4):
            run(i, "subpool")

        time.sleep(0.5)

        pools_path = scheduler_orchid_default_pool_tree_path() + "/pools"
        wait(lambda: get(pools_path + "/subpool/running_operation_count") == 1)
        wait(lambda: get(pools_path + "/subpool/operation_count") == 3)

        wait(lambda: get(pools_path + "/research/running_operation_count") == 1)
        wait(lambda: get(pools_path + "/research/operation_count") == 3)

        assert get(pools_path + "/production/running_operation_count") == 0
        assert get(pools_path + "/production/operation_count") == 0

        move("//sys/pools/research/subpool", "//sys/pools/production/subpool")

        time.sleep(0.5)

        assert get(pools_path + "/subpool/running_operation_count") == 1
        assert get(pools_path + "/subpool/operation_count") == 3

        wait(lambda: get(pools_path + "/research/running_operation_count") == 0)
        wait(lambda: get(pools_path + "/research/operation_count") == 0)

        wait(lambda: get(pools_path + "/production/running_operation_count") == 1)
        wait(lambda: get(pools_path + "/production/operation_count") == 3)

    def _test_pool_acl_prologue(self):
        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out")
        create_user("u")

    def _test_pool_acl_core(self, pool, acl_path):
        def _run_op():
            map(
                command="cat",
                in_="//tmp/t_in",
                out="//tmp/t_out",
                authenticated_user="u",
                spec={"pool": pool},
            )

        _run_op()
        set("//sys/pools{0}/@acl/0/action".format(acl_path), "deny")
        check_permission("u", "use", "//sys/pools" + acl_path)
        with pytest.raises(YtError):
            _run_op()

    @authors("ignat")
    def test_global_pool_acl(self):
        self._test_pool_acl_prologue()
        create_pool(
            "p",
            attributes={"inherit_acl": False, "acl": [make_ace("allow", "u", "use")]},
        )
        self._test_pool_acl_core("p", "/p")

    @authors("ignat")
    def test_inner_pool_acl(self):
        self._test_pool_acl_prologue()
        create_pool(
            "p1",
            attributes={"inherit_acl": False, "acl": [make_ace("allow", "u", "use")]},
        )
        create_pool("p2", parent_name="p1")
        self._test_pool_acl_core("p2", "/p1")

    @authors("ignat")
    def test_forbid_immediate_operations(self):
        self._test_pool_acl_prologue()

        create_pool("p1", attributes={"forbid_immediate_operations": True})
        create_pool("p2", parent_name="p1")
        create_pool("default_pool", attributes={"forbid_immediate_operations": True})

        with pytest.raises(YtError):
            map(
                command="cat",
                in_="//tmp/t_in",
                out="//tmp/t_out",
                user="u",
                spec={"pool": "p1"},
            )

        map(
            command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            user="u",
            spec={"pool": "p2"},
        )

        map(
            command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            user="u",
            spec={"pool": "p3"},
        )


##################################################################


class TestSchedulerPreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "event_log": {"flush_period": 300, "retry_backoff_time": 300},
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {"event_log": {"flush_period": 300, "retry_backoff_time": 300}}
    }

    def setup_method(self, method):
        super(TestSchedulerPreemption, self).setup_method(method)
        set("//sys/pool_trees/default/@config/preemption_satisfaction_threshold", 0.99)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance", 0.7)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance_limit", 0.9)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 0)
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/job_graceful_interrupt_timeout", 10000)
        time.sleep(0.5)

    @authors("ignat")
    def test_preemption(self):
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 2)
        create("table", "//tmp/t_in")
        for i in xrange(3):
            write_table("<append=true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        op1 = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
            spec={"pool": "fake_pool", "job_count": 3, "locality_timeout": 0},
        )

        pools_path = scheduler_orchid_default_pool_tree_path() + "/pools"
        wait(lambda: exists(pools_path + "/fake_pool"))
        wait(lambda: get(pools_path + "/fake_pool/fair_share_ratio") >= 0.999)
        wait(lambda: get(pools_path + "/fake_pool/usage_ratio") >= 0.999)

        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cell/resource_limits/cpu")
        create_pool("test_pool", attributes={"min_share_resources": {"cpu": total_cpu_limit}})
        op2 = map(
            track=False,
            command="cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
            spec={"pool": "test_pool"},
        )
        op2.track()

        op1.abort()

    @authors("ignat")
    @pytest.mark.parametrize("interruptible", [False, True])
    def test_interrupt_job_on_preemption(self, interruptible):
        set("//sys/pool_trees/default/@config/fair_share_preemption_timeout", 100)
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 2)
        create("table", "//tmp/t_in")
        write_table(
            "//tmp/t_in",
            [{"key": "%08d" % i, "value": "(foo)", "data": "a" * (2 * 1024 * 1024)} for i in range(6)],
            table_writer={"block_size": 1024, "desired_chunk_size": 1024},
        )

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        spec = {
            "pool": "fake_pool",
            "locality_timeout": 0,
            "enable_job_splitting": False,
            "max_failed_job_count": 1,
        }
        if interruptible:
            data_size_per_job = get("//tmp/t_in/@uncompressed_data_size")
            spec["data_size_per_job"] = data_size_per_job / 3 + 1
        else:
            spec["job_count"] = 3

        mapper = " ; ".join([events_on_fs().breakpoint_cmd(), "sleep 7", "cat"])
        op1 = map(
            track=False,
            command=mapper,
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
            spec=spec,
        )

        time.sleep(3)

        pools_path = scheduler_orchid_default_pool_tree_path() + "/pools"
        assert get(pools_path + "/fake_pool/fair_share_ratio") >= 0.999
        assert get(pools_path + "/fake_pool/usage_ratio") >= 0.999

        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cell/resource_limits/cpu")
        create_pool("test_pool", attributes={"min_share_resources": {"cpu": total_cpu_limit}})

        # Ensure that all three jobs have started.
        events_on_fs().wait_breakpoint(timeout=datetime.timedelta(1000), job_count=3)
        events_on_fs().release_breakpoint()

        op2 = map(
            track=False,
            command="cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
            spec={"pool": "test_pool", "max_failed_job_count": 1},
        )
        op2.track()
        op1.track()
        assert get(op1.get_path() + "/@progress/jobs/completed/total") == (4 if interruptible else 3)

    @authors("dakovalkov")
    def test_graceful_preemption(self):
        create_test_tables(row_count=1)

        command = """(trap "sleep 1; echo '{interrupt=42}'; exit 0" SIGINT; BREAKPOINT)"""

        op = map(
            track=False,
            command=with_breakpoint(command),
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "mapper": {
                    "interruption_signal": "SIGINT",
                },
                "preemption_mode": "graceful",
            },
        )

        wait_breakpoint()

        update_op_parameters(op.id, parameters=get_scheduling_options(user_slots=0))
        wait(lambda: op.get_job_count("running") == 0)
        op.track()
        assert op.get_job_count("completed", from_orchid=False) == 1
        assert op.get_job_count("total", from_orchid=False) == 1
        assert read_table("//tmp/t_out") == [{"interrupt": 42}]

    @authors("dakovalkov")
    def test_graceful_preemption_timeout(self):
        op = run_test_vanilla(
            command=with_breakpoint("BREAKPOINT ; sleep 100"),
            spec={"preemption_mode": "graceful"}
        )

        wait_breakpoint()
        release_breakpoint()
        update_op_parameters(op.id, parameters=get_scheduling_options(user_slots=0))
        wait(lambda: op.get_job_count("aborted") == 1)
        assert op.get_job_count("total") == 1
        assert op.get_job_count("aborted") == 1

    @authors("ignat")
    def test_min_share_ratio(self):
        create_pool("test_min_share_ratio_pool", attributes={"min_share_resources": {"cpu": 3}})

        get_operation_min_share_ratio = lambda op_id: get(
            "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/min_share_ratio".format(
                op_id
            )
        )

        min_share_settings = [{"cpu": 3}, {"cpu": 1, "user_slots": 3}]

        for min_share_spec in min_share_settings:
            reset_events_on_fs()
            op = run_test_vanilla(
                with_breakpoint("BREAKPOINT"),
                spec={
                    "pool": "test_min_share_ratio_pool",
                    "min_share_resources": min_share_spec,
                },
                job_count=3,
            )
            wait_breakpoint()

            wait(lambda: get_operation_min_share_ratio(op.id) == 1.0)

            release_breakpoint()
            op.track()

    @authors("ignat")
    def test_recursive_preemption_settings(self):
        create_pool("p1", attributes={"fair_share_starvation_tolerance_limit": 0.6})
        create_pool("p2", parent_name="p1")
        create_pool("p3", parent_name="p1", attributes={"fair_share_starvation_tolerance": 0.5})
        create_pool("p4", parent_name="p1", attributes={"fair_share_starvation_tolerance": 0.9})
        create_pool("p5", attributes={"fair_share_starvation_tolerance": 0.8})
        create_pool("p6", parent_name="p5")

        get_pool_tolerance = lambda pool: get(
            scheduler_orchid_pool_path(pool) + "/adjusted_fair_share_starvation_tolerance"
        )

        wait(lambda: get_pool_tolerance("p1") == 0.7)
        wait(lambda: get_pool_tolerance("p2") == 0.6)
        wait(lambda: get_pool_tolerance("p3") == 0.5)
        wait(lambda: get_pool_tolerance("p4") == 0.6)
        wait(lambda: get_pool_tolerance("p5") == 0.8)
        wait(lambda: get_pool_tolerance("p6") == 0.8)

        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})
        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")
        create("table", "//tmp/t_out3")
        create("table", "//tmp/t_out4")

        op1 = map(
            track=False,
            command="sleep 1000; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out1",
            spec={"pool": "p2", "fair_share_starvation_tolerance": 0.4},
        )

        op2 = map(
            track=False,
            command="sleep 1000; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out2",
            spec={"pool": "p2", "fair_share_starvation_tolerance": 0.8},
        )

        op3 = map(
            track=False,
            command="sleep 1000; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out3",
            spec={"pool": "p6"},
        )

        op4 = map(
            track=False,
            command="sleep 1000; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out4",
            spec={"pool": "p6", "fair_share_starvation_tolerance": 0.9},
        )

        get_operation_tolerance = lambda op_id: get(
            "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/adjusted_fair_share_starvation_tolerance".format(
                op_id
            )
        )

        wait(lambda: get_operation_tolerance(op1.id) == 0.4)
        wait(lambda: get_operation_tolerance(op2.id) == 0.6)
        wait(lambda: get_operation_tolerance(op3.id) == 0.8)
        wait(lambda: get_operation_tolerance(op4.id) == 0.9)

        op1.abort()
        op2.abort()

    @authors("asaitgalin", "ignat")
    def test_preemption_of_jobs_excessing_resource_limits(self):
        create("table", "//tmp/t_in")
        for i in xrange(3):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out")

        op = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            spec={"data_size_per_job": 1},
        )

        wait(lambda: len(op.get_running_jobs()) == 3)

        update_op_parameters(
            op.id,
            parameters={"scheduling_options_per_pool_tree": {"default": {"resource_limits": {"user_slots": 1}}}},
        )

        wait(lambda: len(op.get_running_jobs()) == 1)

        update_op_parameters(
            op.id,
            parameters={"scheduling_options_per_pool_tree": {"default": {"resource_limits": {"user_slots": 0}}}},
        )

        wait(lambda: len(op.get_running_jobs()) == 0)

    @authors("mrkastep")
    def test_preemptor_event_log(self):
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 2)
        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cell/resource_limits/cpu")
        create_pool("pool1", attributes={"min_share_resources": {"cpu": total_cpu_limit}})

        create("table", "//tmp/t_in")
        create("table", "//tmp/t_out0")
        create("table", "//tmp/t_out1")

        for i in xrange(3):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        op0 = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 1000; cat", breakpoint_name="b0"),
            in_="//tmp/t_in",
            out="//tmp/t_out0",
            spec={"pool": "pool0", "job_count": 3},
        )

        wait_breakpoint(breakpoint_name="b0", job_count=3)
        release_breakpoint(breakpoint_name="b0")

        op1 = map(
            track=False,
            command=with_breakpoint("cat; BREAKPOINT", breakpoint_name="b1"),
            in_="//tmp/t_in",
            out="//tmp/t_out1",
            spec={"pool": "pool1", "job_count": 1},
        )

        preemptor_job_id = wait_breakpoint(breakpoint_name="b1")[0]
        release_breakpoint(breakpoint_name="b1")

        def check_events():
            for row in read_table("//sys/scheduler/event_log"):
                event_type = row["event_type"]
                if event_type == "job_aborted" and row["operation_id"] == op0.id:
                    assert row["preempted_for"]["operation_id"] == op1.id
                    assert row["preempted_for"]["job_id"] == preemptor_job_id
                    return True
            return False

        wait(lambda: check_events())

        op0.abort()


class TestSchedulingBugOfOperationWithGracefulPreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {"exec_agent": {"job_controller": {"resource_limits": {"cpu": 2, "user_slots": 2}}}}

    @authors("renadeen")
    def test_scheduling_bug_of_operation_with_graceful_preemption(self):
        # Scenario:
        # 1. run operation with graceful preemption and with two jobs;
        # 2. first job is successfully scheduled;
        # 3. operation status becomes Normal since usage/fair_share is greater than fair_share_starvation_tolerance;
        # 4. next job will never be scheduled cause scheduler doesn't schedule jobs for operations with graceful preemption and Normal status.

        # TODO(renadeen): need this placeholder operation to work around some bugs in scheduler (YT-13840).
        run_test_vanilla(with_breakpoint("BREAKPOINT", breakpoint_name="placeholder"))

        op = run_test_vanilla(
            command=with_breakpoint("BREAKPOINT", breakpoint_name="graceful"),
            job_count=2,
            spec={
                "preemption_mode": "graceful",
                "fair_share_starvation_tolerance": 0.4
            }
        )

        wait_breakpoint(breakpoint_name="graceful", job_count=1)
        release_breakpoint(breakpoint_name="placeholder")

        wait_breakpoint(breakpoint_name="graceful", job_count=2)
        wait(lambda: op.get_job_count("running") == 2)
        release_breakpoint(breakpoint_name="graceful")
        op.track()


##################################################################


class TestInferWeightFromMinShare(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {"scheduler": {"fair_share_update_period": 100}}

    @authors("ignat")
    def test_infer_weight_from_min_share(self):
        total_cpu_limit = get("//sys/scheduler/orchid/scheduler/cluster/resource_limits/cpu")
        set("//sys/pool_trees/default/@config/infer_weight_from_min_share_ratio_multiplier", 10)

        create_pool(
            "test_pool1",
            pool_tree="default",
            attributes={"min_share_resources": {"cpu": 0.3 * total_cpu_limit}},
        )
        create_pool(
            "test_pool2",
            pool_tree="default",
            attributes={"min_share_resources": {"cpu": 0.4 * total_cpu_limit}},
        )
        create_pool("test_pool3", pool_tree="default")
        create_pool(
            "subpool1",
            pool_tree="default",
            parent_name="test_pool2",
            attributes={"min_share_resources": {"cpu": 0.4 * 0.3 * total_cpu_limit}},
        )
        create_pool(
            "subpool2",
            pool_tree="default",
            parent_name="test_pool2",
            attributes={"min_share_resources": {"cpu": 0.4 * 0.4 * total_cpu_limit}},
        )

        get_pool_weight = lambda pool: get(scheduler_orchid_pool_path(pool) + "/weight")

        wait(lambda: are_almost_equal(get_pool_weight("test_pool1"), 3.0 / 7.0 * 10.0))
        wait(lambda: are_almost_equal(get_pool_weight("test_pool2"), 4.0 / 7.0 * 10.0))
        wait(lambda: are_almost_equal(get_pool_weight("test_pool3"), 1.0))

        wait(lambda: are_almost_equal(get_pool_weight("subpool1"), 3.0))
        wait(lambda: are_almost_equal(get_pool_weight("subpool2"), 4.0))


##################################################################


class TestResourceLimitsOverdraftPreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 2
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "allowed_node_resources_overcommit_duration": 1000,
        }
    }

    DELTA_NODE_CONFIG = {"exec_agent": {"job_controller": {"resource_limits": {"cpu": 2, "user_slots": 2}}}}

    def setup(self):
        set("//sys/pool_trees/default/@config/job_graceful_interrupt_timeout", 10000)
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 600000)

    def teardown(self):
        remove("//sys/scheduler/config", force=True)

    @authors("ignat")
    def test_scheduler_preempt_overdraft_resources(self):
        set("//sys/pool_trees/default/@config/job_interrupt_timeout", 1000)

        nodes = ls("//sys/cluster_nodes")
        assert len(nodes) > 0

        set(
            "//sys/cluster_nodes/{}/@resource_limits_overrides".format(nodes[0]),
            {"cpu": 0},
        )
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/cpu".format(nodes[0])) == 0.0)

        create("table", "//tmp/t_in")
        for i in xrange(1):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        op1 = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 1000; cat"),
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
        )
        wait_breakpoint()

        op2 = map(
            track=False,
            command=with_breakpoint("BREAKPOINT; sleep 1000; cat"),
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
        )
        wait_breakpoint()

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_usage/cpu".format(nodes[1])) == 2.0)

        # TODO(ignat): add check that jobs are not preemptable.

        set(
            "//sys/cluster_nodes/{}/@resource_limits_overrides".format(nodes[0]),
            {"cpu": 2},
        )
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/cpu".format(nodes[0])) == 2.0)

        set(
            "//sys/cluster_nodes/{}/@resource_limits_overrides".format(nodes[1]),
            {"cpu": 0},
        )
        wait(lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/cpu".format(nodes[1])) == 0.0)

        wait(lambda: op1.get_job_count("aborted") == 1)
        wait(lambda: op2.get_job_count("aborted") == 1)

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)

    @authors("ignat")
    def test_scheduler_force_abort(self):
        nodes = ls("//sys/cluster_nodes")
        assert len(nodes) >= 2

        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(nodes[0]), True)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(nodes[0])) == 0
        )

        create("table", "//tmp/t_in")
        for i in xrange(1):
            write_table("<append=%true>//tmp/t_in", {"foo": "bar"})

        create("table", "//tmp/t_out1")
        create("table", "//tmp/t_out2")

        op1 = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out1",
        )
        op2 = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out="//tmp/t_out2",
        )

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_usage/user_slots".format(nodes[1])) == 2
        )

        # TODO(ignat): add check that jobs are not preemptable.

        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(nodes[0]), False)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(nodes[0])) == 2
        )

        set("//sys/cluster_nodes/{}/@disable_scheduler_jobs".format(nodes[1]), True)
        wait(
            lambda: get("//sys/cluster_nodes/{}/orchid/job_controller/resource_limits/user_slots".format(nodes[1])) == 0
        )

        wait(lambda: op1.get_job_count("aborted") == 1)
        wait(lambda: op2.get_job_count("aborted") == 1)

        wait(lambda: op1.get_job_count("running") == 1)
        wait(lambda: op2.get_job_count("running") == 1)


##################################################################


class TestSchedulerHangingOperations(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "operation_hangup_check_period": 100,
            "operation_hangup_safe_timeout": 5000,
            "operation_hangup_min_schedule_job_attempts": 10,
            "operation_hangup_due_to_limiting_ancestor_safe_timeout": 5000,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "available_exec_nodes_check_period": 1000,
            "banned_exec_nodes_check_period": 100000000,
        }
    }

    @classmethod
    def modify_node_config(cls, config):
        config["exec_agent"]["job_controller"]["resource_limits"]["cpu"] = 2
        config["exec_agent"]["job_controller"]["resource_limits"]["user_slots"] = 2

    def setup_method(self, method):
        super(TestSchedulerHangingOperations, self).setup_method(method)
        # TODO(eshcherbin): Remove this after tree config is reset correctly in yt_env_setup.
        set("//sys/pool_trees/default/@config/enable_limiting_ancestor_check", True)
        wait(lambda: get(scheduler_orchid_pool_tree_config_path("default") + "/enable_limiting_ancestor_check"))

    @authors("ignat")
    def test_hanging_operations(self):
        create("table", "//tmp/t_in")
        write_table("<append=true>//tmp/t_in", {"foo": "bar"})

        ops = []
        for i in xrange(5):
            table = "//tmp/t_out" + str(i)
            create("table", table)
            op = map(
                track=False,
                command="sleep 1000; cat",
                in_=["//tmp/t_in"],
                out=table,
                spec={
                    "pool": "fake_pool",
                    "locality_timeout": 0,
                    "mapper": {"cpu_limit": 0.8},
                },
            )
            ops.append(op)

        for op in ops:
            wait(lambda: len(op.get_running_jobs()) == 1)

        table = "//tmp/t_out_other"
        create("table", table)
        op = map(
            track=False,
            command="sleep 1000; cat",
            in_=["//tmp/t_in"],
            out=table,
            spec={
                "pool": "fake_pool",
                "locality_timeout": 0,
                "mapper": {"cpu_limit": 1.5},
            },
        )

        wait(lambda: op.get_state() == "failed")

        result = str(get(op.get_path() + "/@result"))
        assert "scheduling is hanged" in result
        assert "no successful scheduled jobs" in result

    @authors("eshcherbin")
    def test_disable_limiting_ancestor_check_for_operation(self):
        create_pool("limiting_pool", attributes={"resource_limits": {"cpu": 1.0}})
        create_pool("subpool", parent_name="limiting_pool")
        wait(lambda: get(scheduler_orchid_pool_path("limiting_pool") + "/resource_limits/cpu") == 1.0)

        # NB(eshcherbin): We set the pool in "scheduling_options_per_pool_tree" to also cover the problem from YT-13761.
        op = run_test_vanilla(
            "sleep 0.5",
            job_count=10,
            spec={
                "enable_limiting_ancestor_check": False,
                "scheduling_options_per_pool_tree": {
                    "default": {
                        "pool": "subpool",
                    },
                },
            },
            task_patch={"cpu_limit": 2.0},
        )

        # Let the operation hang for some time. The limiting ancestor check should not be triggered.
        time.sleep(10)

        remove("//sys/pool_trees/default/limiting_pool/@resource_limits")
        op.track()

    @authors("eshcherbin")
    def test_disable_limiting_ancestor_check_for_tree(self):
        set("//sys/pool_trees/default/@config/enable_limiting_ancestor_check", False)
        wait(lambda: not get(scheduler_orchid_pool_tree_config_path("default") + "/enable_limiting_ancestor_check"))

        create_pool("limiting_pool", attributes={"resource_limits": {"cpu": 1.0}})
        create_pool("subpool", parent_name="limiting_pool")
        wait(lambda: get(scheduler_orchid_pool_path("limiting_pool") + "/resource_limits/cpu") == 1.0)

        op = run_test_vanilla(
            "sleep 0.5",
            job_count=10,
            spec={"pool": "subpool"},
            task_patch={"cpu_limit": 2.0},
        )

        # Let the operation hang for some time. The limiting ancestor check should not be triggered.
        time.sleep(10)

        remove("//sys/pool_trees/default/limiting_pool/@resource_limits")
        op.track()

    @authors("eshcherbin")
    def test_limiting_ancestor(self):
        create_pool("limiting_pool", attributes={"resource_limits": {"cpu": 1.0}})
        create_pool("subpool", parent_name="limiting_pool")
        wait(lambda: get(scheduler_orchid_pool_path("limiting_pool") + "/resource_limits/cpu") == 1.0)

        op = run_sleeping_vanilla(job_count=10, spec={"pool": "subpool"}, task_patch={"cpu_limit": 2.0})

        wait(lambda: op.get_state() == "failed")

        result = str(get(op.get_path() + "/@result"))
        assert "scheduling is hanged" in result
        assert "limiting_ancestor" in result and "limiting_pool" in result

    @authors("eshcherbin")
    def test_skip_limiting_ancestor_check_on_node_shortage(self):
        set("//sys/scheduler/config/operation_hangup_safe_timeout", 100000000)
        wait(
            lambda: get(scheduler_orchid_path() + "/scheduler/config/operation_hangup_safe_timeout"),
            100000000,
        )

        set("//sys/pool_trees/default/@config/nodes_filter", "!custom")
        create_pool_tree("custom_tree", config={"nodes_filter": "custom"})
        create_pool("research", pool_tree="custom_tree")

        node = ls("//sys/cluster_nodes")[0]
        set("//sys/cluster_nodes/{}/@user_tags".format(node), ["custom"])
        wait(
            lambda: get(
                scheduler_orchid_path() + "/scheduler/scheduling_info_per_pool_tree/custom_tree/resource_limits/cpu"
            )
            > 0.0
        )

        op = run_test_vanilla(
            with_breakpoint("BREAKPOINT"),
            job_count=2,
            spec={"pool": "subpool", "pool_trees": ["custom_tree"]},
            task_patch={"cpu_limit": 2.0},
        )
        wait_breakpoint()

        # Ensure that the controller has seen the available node and will not fail the operation.
        time.sleep(3.0)

        set("//sys/cluster_nodes/{}/@user_tags".format(node), [])
        wait(
            lambda: get(
                scheduler_orchid_path() + "/scheduler/scheduling_info_per_pool_tree/custom_tree/resource_limits/cpu"
            )
            == 0.0
        )

        release_breakpoint()

        # Let the operation hang with no available nodes in the tree for some time.
        # The limiting ancestor check should not be triggered.
        time.sleep(10)

        set("//sys/cluster_nodes/{}/@user_tags".format(node), ["custom"])
        wait(
            lambda: get(
                scheduler_orchid_path() + "/scheduler/scheduling_info_per_pool_tree/custom_tree/resource_limits/cpu"
            )
            > 0.0
        )

        op.track()


##################################################################


class TestSchedulerAggressivePreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 4
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {"scheduler": {"fair_share_update_period": 100}}

    def setup_method(self, method):
        super(TestSchedulerAggressivePreemption, self).setup_method(method)
        set("//sys/pool_trees/default/@config/aggressive_preemption_satisfaction_threshold", 0.2)
        set("//sys/pool_trees/default/@config/preemption_satisfaction_threshold", 1.0)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance", 0.9)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 0)
        set("//sys/pool_trees/default/@config/fair_share_preemption_timeout", 100)
        set("//sys/pool_trees/default/@config/fair_share_preemption_timeout_limit", 100)
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 5)
        time.sleep(0.5)

    @classmethod
    def modify_node_config(cls, config):
        for resource in ["cpu", "user_slots"]:
            config["exec_agent"]["job_controller"]["resource_limits"][resource] = 2

    @authors("ignat")
    def test_aggressive_preemption(self):
        create_pool("special_pool")
        set("//sys/pools/special_pool/@aggressive_starvation_enabled", True)
        scheduling_info_path = (
            "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/"
        )

        def get_fair_share_ratio(op_id):
            return get(scheduling_info_path.format(op_id) + "fair_share_ratio")

        def get_usage_ratio(op_id):
            return get(scheduling_info_path.format(op_id) + "usage_ratio")

        ops = []
        for index in xrange(2):
            create("table", "//tmp/t_out" + str(index))
            op = run_sleeping_vanilla(
                job_count=4,
                spec={"pool": "fake_pool" + str(index), "locality_timeout": 0},
            )
            ops.append(op)
        time.sleep(3)

        for op in ops:
            wait(lambda: are_almost_equal(get_fair_share_ratio(op.id), 1.0 / 2.0))
            wait(lambda: are_almost_equal(get_usage_ratio(op.id), 1.0 / 2.0))
            wait(lambda: len(op.get_running_jobs()) == 4)

        op = run_sleeping_vanilla(
            job_count=1,
            spec={"pool": "special_pool", "locality_timeout": 0},
            task_patch={"cpu_limit": 2},
        )
        time.sleep(3)

        wait(lambda: are_almost_equal(get_fair_share_ratio(op.id), 1.0 / 4.0))
        wait(lambda: are_almost_equal(get_usage_ratio(op.id), 1.0 / 4.0))
        wait(lambda: len(op.get_running_jobs()) == 1)

    @authors("eshcherbin")
    def test_no_aggressive_preemption_for_non_aggressively_starving_operation(self):
        create_pool("fake_pool", attributes={"weight": 10.0})
        create_pool("regular_pool")
        create_pool("special_pool", attributes={"weight": 2.0})

        scheduling_info_path = (
            "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/"
        )

        def is_starving(op_id):
            return get(scheduling_info_path.format(op_id) + "starving")

        def get_fair_share_ratio(op_id):
            return get(scheduling_info_path.format(op_id) + "fair_share_ratio")

        def get_usage_ratio(op_id):
            return get(scheduling_info_path.format(op_id) + "usage_ratio")

        bad_op = run_sleeping_vanilla(
            job_count=8,
            spec={
                "pool": "fake_pool",
                "locality_timeout": 0,
                "update_preemptable_jobs_list_logging_period": 1,
            },
        )
        time.sleep(3)

        wait(lambda: are_almost_equal(get_fair_share_ratio(bad_op.id), 1.0))
        wait(lambda: are_almost_equal(get_usage_ratio(bad_op.id), 1.0))
        wait(lambda: len(bad_op.get_running_jobs()) == 8)

        op1 = run_sleeping_vanilla(job_count=2, spec={"pool": "special_pool", "locality_timeout": 0})
        time.sleep(3)

        wait(lambda: are_almost_equal(get_fair_share_ratio(op1.id), 2.0 / 12.0))
        wait(lambda: are_almost_equal(get_usage_ratio(op1.id), 1.0 / 8.0))
        wait(lambda: len(op1.get_running_jobs()) == 1)

        time.sleep(3)
        wait(lambda: are_almost_equal(get_usage_ratio(op1.id), 1.0 / 8.0))
        wait(lambda: len(op1.get_running_jobs()) == 1)

        op2 = run_sleeping_vanilla(job_count=1, spec={"pool": "regular_pool", "locality_timeout": 0})
        time.sleep(3)

        wait(lambda: is_starving(op2.id))
        wait(lambda: are_almost_equal(get_fair_share_ratio(op2.id), 1.0 / 13.0))
        wait(lambda: are_almost_equal(get_usage_ratio(op2.id), 0.0))
        wait(lambda: len(op2.get_running_jobs()) == 0)

        time.sleep(3)
        wait(lambda: are_almost_equal(get_usage_ratio(op2.id), 0.0))
        wait(lambda: len(op2.get_running_jobs()) == 0)

        set("//sys/pools/special_pool/@aggressive_starvation_enabled", True)

        wait(lambda: are_almost_equal(get_usage_ratio(op1.id), 0.25))
        wait(lambda: len(op1.get_running_jobs()) == 2)


##################################################################


class TestSchedulerAggressiveStarvationPreemption(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {"scheduler": {"fair_share_update_period": 100}}

    def setup_method(self, method):
        super(TestSchedulerAggressiveStarvationPreemption, self).setup_method(method)
        set("//sys/pool_trees/default/@config/aggressive_preemption_satisfaction_threshold", 0.2)
        set("//sys/pool_trees/default/@config/preemption_satisfaction_threshold", 0.75)
        set("//sys/pool_trees/default/@config/preemption_check_starvation", False)
        set("//sys/pool_trees/default/@config/preemption_check_satisfaction", False)
        set("//sys/pool_trees/default/@config/fair_share_preemption_timeout", 100)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 2)
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        time.sleep(0.5)

    @classmethod
    def modify_node_config(cls, config):
        config["exec_agent"]["job_controller"]["resource_limits"]["cpu"] = 5
        config["exec_agent"]["job_controller"]["resource_limits"]["user_slots"] = 5

    @authors("eshcherbin")
    def test_allow_aggressive_starvation_preemption_operation(self):
        get_fair_share_ratio = lambda op_id: get(
            scheduler_orchid_operation_path(op_id) + "/fair_share_ratio", default=0.0
        )
        get_usage_ratio = lambda op_id: get(scheduler_orchid_operation_path(op_id) + "/usage_ratio", default=0.0)

        create_pool("honest_pool", attributes={"min_share_resources": {"cpu": 15}})
        create_pool(
            "honest_subpool_big",
            parent_name="honest_pool",
            attributes={"min_share_resources": {"cpu": 10}},
        )
        create_pool(
            "honest_subpool_small",
            parent_name="honest_pool",
            attributes={"min_share_resources": {"cpu": 5}},
        )
        create_pool("dishonest_pool")
        create_pool(
            "special_pool",
            attributes={
                "min_share_resources": {"cpu": 10},
                "aggressive_starvation_enabled": True,
            },
        )

        op_dishonest = run_sleeping_vanilla(spec={"pool": "dishonest_pool"}, task_patch={"cpu_limit": 5}, job_count=2)
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_dishonest.id), 0.4))
        wait(lambda: are_almost_equal(get_usage_ratio(op_dishonest.id), 0.4))

        op_honest_small = run_sleeping_vanilla(spec={"pool": "honest_subpool_small"}, task_patch={"cpu_limit": 5})
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_small.id), 0.2))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_small.id), 0.2))

        op_honest_big = run_sleeping_vanilla(
            spec={
                "pool": "honest_subpool_big",
                "allow_aggressive_starvation_preemption": False,
            },
            job_count=20,
        )
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.6))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.4))

        op_special = run_sleeping_vanilla(spec={"pool": "special_pool"}, job_count=10)

        wait(lambda: are_almost_equal(get_fair_share_ratio(op_special.id), 0.4))
        time.sleep(1.0)
        wait(lambda: are_almost_equal(get_usage_ratio(op_special.id), 0.08), iter=10)
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.4))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.32))

    @pytest.mark.skip("This test either is incorrect or simply doesn't work until YT-13715 is resolved")
    @authors("eshcherbin")
    def test_allow_aggressive_starvation_preemption_ancestor(self):
        get_fair_share_ratio = lambda op_id: get(
            scheduler_orchid_operation_path(op_id) + "/fair_share_ratio", default=0.0
        )
        get_usage_ratio = lambda op_id: get(scheduler_orchid_operation_path(op_id) + "/usage_ratio", default=0.0)

        create_pool(
            "honest_pool",
            attributes={
                "min_share_resources": {"cpu": 15},
                "allow_aggressive_starvation_preemption": False,
            },
        )
        create_pool(
            "honest_subpool_big",
            parent_name="honest_pool",
            attributes={"min_share_resources": {"cpu": 10}},
        )
        create_pool(
            "honest_subpool_small",
            parent_name="honest_pool",
            attributes={"min_share_resources": {"cpu": 5}},
        )
        create_pool("dishonest_pool")
        create_pool(
            "special_pool",
            attributes={
                "min_share_resources": {"cpu": 10},
                "aggressive_starvation_enabled": True,
            },
        )

        op_dishonest = run_sleeping_vanilla(spec={"pool": "dishonest_pool"}, task_patch={"cpu_limit": 5}, job_count=2)
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_dishonest.id), 0.4))
        wait(lambda: are_almost_equal(get_usage_ratio(op_dishonest.id), 0.4))

        op_honest_small = run_sleeping_vanilla(spec={"pool": "honest_subpool_small"}, task_patch={"cpu_limit": 5})
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_small.id), 0.2))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_small.id), 0.2))

        op_honest_big = run_sleeping_vanilla(
            spec={
                "pool": "honest_subpool_big",
                "scheduling_options_per_pool_tree": {"default": {"enable_detailed_logs": True}},
            },
            job_count=20,
        )
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.6))
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.4))

        op_special = run_sleeping_vanilla(spec={"pool": "special_pool"}, job_count=10)

        wait(lambda: are_almost_equal(get_fair_share_ratio(op_special.id), 0.4))
        wait(lambda: are_almost_equal(get_fair_share_ratio(op_honest_big.id), 0.4))
        time.sleep(1.0)
        wait(lambda: are_almost_equal(get_usage_ratio(op_special.id), 0.08), iter=10)
        wait(lambda: are_almost_equal(get_usage_ratio(op_honest_big.id), 0.32))


##################################################################


class TestSchedulerPoolsCommon(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "fair_share_update_period": 300,
            "profiling_update_period": 300,
            "fair_share_profiling_period": 300,
            "event_log": {"flush_period": 300, "retry_backoff_time": 300},
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {"event_log": {"flush_period": 300, "retry_backoff_time": 300}}
    }

    @authors("ignat")
    def test_pools_reconfiguration(self):
        create_test_tables(attributes={"replication_factor": 1})

        testing_options = {"scheduling_delay": 1000}

        create_pool("test_pool_1")
        create_pool("test_pool_2")

        op = map(
            track=False,
            command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"pool": "test_pool_1", "testing": testing_options},
        )
        time.sleep(1)

        # TODO(ignat): Make this more stable.
        remove("//sys/pools/test_pool_1")
        create_pool("test_pool_1", parent_name="test_pool_2", wait_for_orchid=False)

        op.track()

    @authors("ignat")
    def test_default_parent_pool(self):
        create("table", "//tmp/t_in")
        set("//tmp/t_in/@replication_factor", 1)
        write_table("//tmp/t_in", {"foo": "bar"})

        for output in ["//tmp/t_out1", "//tmp/t_out2"]:
            create("table", output)
            set(output + "/@replication_factor", 1)

        create_pool("default_pool")
        set("//sys/pool_trees/default/@config/default_parent_pool", "default_pool")
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 2)
        time.sleep(0.2)

        command = with_breakpoint("cat ; BREAKPOINT")
        op1 = map(track=False, command=command, in_="//tmp/t_in", out="//tmp/t_out1")

        op2 = map(
            track=False,
            command=command,
            in_="//tmp/t_in",
            out="//tmp/t_out2",
            spec={"pool": "my_pool"},
        )
        # Each operation has one job.
        wait_breakpoint(job_count=2)

        pools_path = scheduler_orchid_default_pool_tree_path() + "/pools"
        pool = get(pools_path + "/root")
        assert pool["parent"] == "default_pool"

        pool = get(pools_path + "/my_pool")
        assert pool["parent"] == "default_pool"

        scheduling_info_per_pool_tree = "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree"
        assert __builtin__.set(["root", "my_pool"]) == __builtin__.set(
            get(scheduling_info_per_pool_tree + "/default/user_to_ephemeral_pools/root")
        )

        remove("//sys/pools/default_pool")

        release_breakpoint()
        for op in [op1, op2]:
            op.track()

    @authors("renadeen")
    def test_ephemeral_flag(self):
        create_pool("real_pool")
        op = run_sleeping_vanilla(spec={"pool": "ephemeral_pool"})
        op.wait_for_state("running")
        wait(
            lambda: get(
                scheduler_orchid_pool_path("ephemeral_pool") + "/is_ephemeral",
                default=False,
            )
        )
        wait(lambda: not get(scheduler_orchid_pool_path("real_pool") + "/is_ephemeral", default=False))
        wait(lambda: not get(scheduler_orchid_pool_path("<Root>") + "/is_ephemeral", default=False))

    @authors("renadeen")
    def test_ephemeral_pool_in_custom_pool_simple(self):
        create_pool("custom_pool")
        set("//sys/pools/custom_pool/@create_ephemeral_subpools", True)
        time.sleep(0.2)

        op = run_sleeping_vanilla(spec={"pool": "custom_pool"})
        wait(lambda: len(list(op.get_running_jobs())) == 1)

        wait(lambda: exists(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root"))
        pool = get(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root")
        assert pool["parent"] == "custom_pool"
        assert pool["mode"] == "fair_share"
        assert pool["is_ephemeral"]

    @authors("renadeen")
    def test_custom_ephemeral_pool_persists_after_pool_update(self):
        create_pool("custom_pool")
        set("//sys/pools/custom_pool/@create_ephemeral_subpools", True)
        time.sleep(0.2)

        op = run_sleeping_vanilla(spec={"pool": "custom_pool"})
        wait(lambda: len(list(op.get_running_jobs())) == 1)
        wait(
            lambda: get(
                scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root/parent",
                default="",
            )
            == "custom_pool"
        )

        create_pool("trigger_pool_update")
        time.sleep(0.5)  # wait orchid update
        # after pools update all ephemeral pools where mistakenly moved to default pool
        assert get(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root/parent") == "custom_pool"

    @authors("renadeen")
    def test_ephemeral_pool_parent_is_removed_after_operation_complete(self):
        create_pool("custom_pool")
        set("//sys/pools/custom_pool/@create_ephemeral_subpools", True)
        time.sleep(0.2)

        op = run_test_vanilla(with_breakpoint("BREAKPOINT"), spec={"pool": "custom_pool"})
        wait_breakpoint()

        remove("//sys/pools/custom_pool")
        wait(lambda: exists(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool"))
        wait(lambda: exists(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root"))

        release_breakpoint()
        op.track()
        wait(lambda: not exists(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool"))
        wait(lambda: not exists(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root"))

    @authors("renadeen")
    def test_custom_ephemeral_pool_scheduling_mode(self):
        create_pool("custom_pool_fifo")
        set("//sys/pools/custom_pool_fifo/@create_ephemeral_subpools", True)
        set("//sys/pools/custom_pool_fifo/@ephemeral_subpool_config", {"mode": "fifo"})
        time.sleep(0.2)

        op = run_sleeping_vanilla(spec={"pool": "custom_pool_fifo"})
        wait(lambda: len(list(op.get_running_jobs())) == 1)

        wait(lambda: exists(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool_fifo$root"))
        pool_fifo = get(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool_fifo$root")
        assert pool_fifo["parent"] == "custom_pool_fifo"
        assert pool_fifo["mode"] == "fifo"

    @authors("renadeen")
    def test_custom_ephemeral_pool_max_operation_count(self):
        create_pool("custom_pool")
        set("//sys/pools/custom_pool/@create_ephemeral_subpools", True)
        set(
            "//sys/pools/custom_pool/@ephemeral_subpool_config",
            {"max_operation_count": 1},
        )
        time.sleep(0.2)

        op = run_sleeping_vanilla(spec={"pool": "custom_pool"})
        wait(lambda: len(list(op.get_running_jobs())) == 1)

        with pytest.raises(YtError):
            run_test_vanilla(command="", spec={"pool": "custom_pool"}, track=True)

    @authors("renadeen")
    def test_custom_ephemeral_pool_resource_limits(self):
        create_pool("custom_pool")
        set("//sys/pools/custom_pool/@create_ephemeral_subpools", True)
        set(
            "//sys/pools/custom_pool/@ephemeral_subpool_config",
            {"resource_limits": {"cpu": 1}},
        )
        time.sleep(0.2)

        run_sleeping_vanilla(spec={"pool": "custom_pool"})
        wait(lambda: exists(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root"))
        pool_info = get(scheduler_orchid_default_pool_tree_path() + "/pools/custom_pool$root")
        assert pool_info["resource_limits"]["cpu"] == 1.0

    @authors("ignat")
    def test_ephemeral_pools_limit(self):
        create("table", "//tmp/t_in")
        set("//tmp/t_in/@replication_factor", 1)
        write_table("//tmp/t_in", {"foo": "bar"})

        for i in xrange(1, 5):
            output = "//tmp/t_out" + str(i)
            create("table", output)
            set(output + "/@replication_factor", 1)

        create_pool("default_pool")
        set("//sys/pool_trees/default/@config/default_parent_pool", "default_pool")
        set("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user", 3)
        time.sleep(0.2)

        ops = []
        breakpoints = []
        for i in xrange(1, 4):
            breakpoint_name = "breakpoint{0}".format(i)
            breakpoints.append(breakpoint_name)
            ops.append(
                map(
                    track=False,
                    command="cat ; {breakpoint_cmd}".format(
                        breakpoint_cmd=events_on_fs().breakpoint_cmd(breakpoint_name)
                    ),
                    in_="//tmp/t_in",
                    out="//tmp/t_out" + str(i),
                    spec={"pool": "pool" + str(i)},
                )
            )
            wait_breakpoint(breakpoint_name)

        scheduling_info_per_pool_tree = "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree"
        assert __builtin__.set(["pool" + str(i) for i in xrange(1, 4)]) == __builtin__.set(
            get(scheduling_info_per_pool_tree + "/default/user_to_ephemeral_pools/root")
        )

        with pytest.raises(YtError):
            map(
                track=False,
                command="cat",
                in_="//tmp/t_in",
                out="//tmp/t_out4",
                spec={"pool": "pool4"},
            )

        remove("//sys/pools/default_pool")
        remove("//sys/pool_trees/default/@config/default_parent_pool")
        remove("//sys/pool_trees/default/@config/max_ephemeral_pools_per_user")

        for breakpoint_name in breakpoints:
            release_breakpoint(breakpoint_name)

        for op in ops:
            op.track()

    @authors("renadeen", "babenko")
    def test_event_log(self):
        create_test_tables(attributes={"replication_factor": 1})

        create_pool("event_log_test_pool", attributes={"min_share_resources": {"cpu": 1}})
        op = map(
            command="cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"pool": "event_log_test_pool"},
        )

        def check_events():
            events = []
            for row in read_table("//sys/scheduler/event_log"):
                event_type = row["event_type"]
                if (
                    event_type.startswith("operation_")
                    and event_type != "operation_prepared"
                    and event_type != "operation_materialized"
                    and row["operation_id"] == op.id
                ):
                    events.append(row["event_type"])
                    if event_type == "operation_started":
                        assert row["pool"]
                    if event_type == "operation_completed":
                        assert row["progress"]["job_statistics"]
            return events == ["operation_started", "operation_completed"]

        wait(lambda: check_events())

        def check_pools():
            pools_info = [
                row
                for row in read_table("//sys/scheduler/event_log")
                if row["event_type"] == "pools_info" and "event_log_test_pool" in row["pools"]["default"]
            ]
            if len(pools_info) != 1:
                return False
            custom_pool_info = pools_info[-1]["pools"]["default"]["event_log_test_pool"]
            assert are_almost_equal(custom_pool_info["min_share_resources"]["cpu"], 1.0)
            assert custom_pool_info["mode"] == "fair_share"
            return True

        wait(lambda: check_pools())

    @authors("eshcherbin")
    def test_pool_count(self):
        def get_orchid_pool_count():
            return get_from_tree_orchid("default", "fair_share_info/pool_count")

        pool_count_last = Metric.at_scheduler("scheduler/pool_count", with_tags={"tree": "default"}, aggr_method="last")

        def check_pool_count(expected_pool_count):
            wait(lambda: get_orchid_pool_count() == expected_pool_count)
            wait(lambda: pool_count_last.update().get(verbose=True) == expected_pool_count)

        check_pool_count(0)

        create_pool("first_pool")
        create_pool("second_pool")
        check_pool_count(2)

        op = run_sleeping_vanilla(spec={"pool": "first_pool"})
        check_pool_count(2)
        op.abort()

        # Ephemeral pool.
        op = run_sleeping_vanilla()
        check_pool_count(3)
        op.abort()
        check_pool_count(2)

    @authors("renadeen")
    def test_ephemeral_pool_name_validation(self):
        with pytest.raises(YtError):
            run_sleeping_vanilla(spec={"pool": "invalid$name"})

        op = run_sleeping_vanilla(spec={"pool": "valid_name"})
        with pytest.raises(YtError):
            update_op_parameters(op.id, parameters={"pool": "invalid|name"})


class TestSchedulerPoolsReconfiguration(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,  # Update pools configuration period
        }
    }

    orchid_pools = scheduler_orchid_default_pool_tree_path() + "/pools"

    def setup_method(self, method):
        super(TestSchedulerPoolsReconfiguration, self).setup_method(method)
        wait(lambda: len(ls(self.orchid_pools)) == 1, sleep_backoff=0.1)  # <Root> is always in orchid
        wait(lambda: not get("//sys/scheduler/@alerts"), sleep_backoff=0.1)

    @authors("renadeen")
    def test_add_nested_pool(self):
        create_pool("test_parent")
        create_pool("test_pool", parent_name="test_parent")

        assert self.get_pool_parent("test_parent") == "<Root>"
        assert self.get_pool_parent("test_pool") == "test_parent"

    @authors("renadeen")
    def test_move_to_existing_pool(self):
        create_pool("test_parent")
        create_pool("test_pool")
        wait(lambda: self.get_pool_parent("test_pool") == "<Root>")

        move("//sys/pools/test_pool", "//sys/pools/test_parent/test_pool")
        wait(lambda: self.get_pool_parent("test_pool") == "test_parent")

    @authors("renadeen")
    def test_move_to_new_pool(self):
        create_pool("test_pool")

        # We'd like to execute these two commands atomically
        create_pool("new_pool", wair_for_orchid=False)
        move("//sys/pools/test_pool", "//sys/pools/new_pool/test_pool")

        self.wait_pool_exists("new_pool")
        wait(lambda: self.get_pool_parent("test_pool") == "new_pool")

    @authors("renadeen")
    def test_move_to_root_pool(self):
        create_pool("test_parent")
        create_pool("test_pool", parent_name="test_parent")
        wait(lambda: self.get_pool_parent("test_pool") == "test_parent")

        move("//sys/pools/test_parent/test_pool", "//sys/pools/test_pool")

        wait(lambda: self.get_pool_parent("test_pool") == "<Root>")

    @authors("renadeen")
    def test_remove_big_hierarchy(self):
        # Test bug when some pools weren't removed due to wrong removal order and inability to remove nonempty pools
        parent = None
        for i in range(10):
            pool = "pool" + str(i)
            create_pool(pool, parent_name=parent)
            parent = pool

        remove("//sys/pools/pool0")
        wait(lambda: len(ls(self.orchid_pools)) == 1)  # only <Root> must remain

    @authors("renadeen")
    def test_subtle_reconfiguration_crash(self):
        # 1. there are two pools - parent (max_running_operation_count=1) and child (max_running_operation_count=2)
        # 2. launch first operation in child - it runs
        # 3. launch second operation in child - it will wait at parent as parent reached its max_running_operation_count
        # 4. set child's max_running_operation_count to 0
        # 5. first operation completes
        # 6. scheduler wakes up second operation
        # 7. operation is stuck in child - the pool where operation just finished - scheduler crash here

        create_pool("parent", attributes={"max_running_operation_count": 1})
        create_pool("child", parent_name="parent", attributes={"max_running_operation_count": 2})
        running_op = run_test_vanilla(with_breakpoint("BREAKPOINT"), job_count=1, spec={"pool": "child"})
        wait_breakpoint()
        waiting_op = run_test_vanilla(with_breakpoint("BREAKPOINT"), job_count=1, spec={"pool": "child"})
        waiting_op.wait_for_state("pending")
        set("//sys/pools/parent/child/@max_running_operation_count", 0)
        release_breakpoint()
        running_op.track()

    @authors("renadeen", "ignat")
    def test_ephemeral_to_explicit_pool_transformation(self):
        create_pool("default_pool", wait_for_orchid=False)
        set("//sys/pool_trees/default/@config/default_parent_pool", "default_pool")
        self.wait_pool_exists("default_pool")

        run_sleeping_vanilla(spec={"pool": "test_pool"})
        self.wait_pool_exists("test_pool")

        create_pool("test_pool")

        wait(lambda: self.get_pool_parent("test_pool") == "<Root>")
        ephemeral_pools = (
            "//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/default/user_to_ephemeral_pools/root"
        )
        wait(lambda: get(ephemeral_pools) == [])

    def wait_pool_exists(self, pool):
        wait(lambda: exists(self.orchid_pools + "/" + pool), sleep_backoff=0.1)

    def get_pool_parent(self, pool):
        return get(self.orchid_pools + "/" + pool + "/parent")

    def wait_and_get_inner_alert_message(self):
        wait(lambda: get("//sys/scheduler/@alerts"))
        return get("//sys/scheduler/@alerts")[0]["inner_errors"][0]["inner_errors"][0]["message"]


##################################################################


class TestSchedulerSuspiciousJobs(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1
    USE_PORTO = True

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {"heartbeat_period": 100},  # 100 msec
            "job_proxy_heartbeat_period": 100,  # 100 msec
            "job_controller": {"resource_limits": {"user_slots": 2, "cpu": 2}},
        }
    }

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "running_jobs_update_period": 100,  # 100 msec
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "suspicious_jobs": {
                "inactivity_timeout": 2000,  # 2 sec
                "update_period": 100,  # 100 msec
            },
        }
    }

    @authors("ignat")
    def test_false_suspicious_jobs(self):
        create("table", "//tmp/t", attributes={"replication_factor": 1})
        create("table", "//tmp/t1", attributes={"replication_factor": 1})
        create("table", "//tmp/t2", attributes={"replication_factor": 1})
        write_table("//tmp/t", [{"foo": i} for i in xrange(10)])

        # Jobs below are not suspicious, they are just stupid.
        op1 = map(
            track=False,
            command='echo -ne "x = 1\nwhile True:\n    x = (x * x + 1) % 424243" | python',
            in_="//tmp/t",
            out="//tmp/t1",
        )

        op2 = map(track=False, command="sleep 1000", in_="//tmp/t", out="//tmp/t2")

        for i in xrange(200):
            running_jobs1 = op1.get_running_jobs()
            running_jobs2 = op2.get_running_jobs()
            print_debug(
                "running_jobs1:",
                len(running_jobs1),
                "running_jobs2:",
                len(running_jobs2),
            )
            if not running_jobs1 or not running_jobs2:
                time.sleep(0.1)
            else:
                break

        if not running_jobs1 or not running_jobs2:
            assert False, "Failed to have running jobs in both operations"

        time.sleep(5)

        job1_id = running_jobs1.keys()[0]
        job2_id = running_jobs2.keys()[0]

        time.sleep(1)

        suspicious1 = get("//sys/scheduler/orchid/scheduler/jobs/{0}/suspicious".format(job1_id))
        suspicious2 = get("//sys/scheduler/orchid/scheduler/jobs/{0}/suspicious".format(job2_id))

        if suspicious1 or suspicious2:
            print_debug("Some of jobs considered suspicious, their brief statistics are:")
            for i in range(50):
                if suspicious1 and exists("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job1_id)):
                    print_debug(
                        "job1 brief statistics:",
                        get("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job1_id)),
                    )
                if suspicious2 and exists("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job2_id)):
                    print_debug(
                        "job2 brief statistics:",
                        get("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job2_id)),
                    )

        assert not suspicious1
        assert not suspicious2

        op1.abort()
        op2.abort()

    @authors("max42")
    def test_true_suspicious_jobs(self):
        create("table", "//tmp/t_in", attributes={"replication_factor": 1})
        create("table", "//tmp/t_out", attributes={"replication_factor": 1})

        for i in range(15):
            write_table("<append=%true>//tmp/t_in", {"a": i})
        op = merge(
            track=False,
            in_=["//tmp/t_in"],
            out="//tmp/t_out",
            spec={
                "force_transform": True,
                "mode": "ordered",
                "job_io": {
                    "testing_options": {"pipe_delay": 4000},
                    "buffer_row_count": 1,
                },
                "enable_job_splitting": False,
            },
        )

        running_jobs = None
        for i in xrange(200):
            running_jobs = op.get_running_jobs()
            print_debug("running_jobs:", len(running_jobs))
            if not running_jobs:
                time.sleep(0.1)
            else:
                break

        if not running_jobs:
            assert False, "Failed to have running jobs"

        time.sleep(5)

        job_id = running_jobs.keys()[0]

        # Most part of the time we should be suspicious, let's check that
        for i in range(30):
            suspicious = get("//sys/scheduler/orchid/scheduler/jobs/{0}/suspicious".format(job_id))
            if suspicious:
                break
            time.sleep(0.1)

        if not suspicious:
            print_debug("Job is not considered suspicious, its brief statistics are:")
            for i in range(50):
                if suspicious and exists("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job_id)):
                    print_debug(
                        "job brief statistics:",
                        get("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job_id)),
                    )

        assert suspicious

    @authors("max42")
    def DISABLED_test_true_suspicious_jobs_old(self):
        # This test involves dirty hack to make lots of retries for fetching feasible
        # seeds from master making the job suspicious (as it doesn't give the input for the
        # user job for a long time).
        #
        # We create a table consisting of the only chunk, temporarily set cpu = 0 to prevent
        # the map from running via @resource_limits_overrides, then we remove the chunk from
        # the chunk_store via the filesystem and return CPU back to the normal state.

        create("table", "//tmp/t", attributes={"replication_factor": 1})
        create("table", "//tmp/d", attributes={"replication_factor": 1})
        write_table("//tmp/t", {"a": 2})

        nodes = ls("//sys/cluster_nodes")
        assert len(nodes) == 1
        node = nodes[0]
        set(
            "//sys/cluster_nodes/{0}/@resource_limits_overrides".format(node),
            {"cpu": 0},
        )

        op = map(track=False, command="cat", in_="//tmp/t", out="//tmp/d")

        chunk_id = get_singular_chunk_id("//tmp/t")

        chunk_store_path = self.Env.configs["node"][0]["data_node"]["store_locations"][0]["path"]
        chunk_path = os.path.join(chunk_store_path, chunk_id[-2:], chunk_id)
        os.remove(chunk_path)
        os.remove(chunk_path + ".meta")

        set(
            "//sys/cluster_nodes/{0}/@resource_limits_overrides".format(node),
            {"cpu": 1},
        )

        while True:
            if exists("//sys/scheduler/orchid/scheduler/operations/{0}".format(op.id)):
                running_jobs = op.get_running_jobs()
                if len(running_jobs) > 0:
                    break

            time.sleep(1.0)

        assert len(running_jobs) == 1
        job_id = running_jobs.keys()[0]

        for i in xrange(20):
            suspicious = get("//sys/scheduler/orchid/scheduler/jobs/{0}/suspicious".format(job_id))
            if not suspicious:
                time.sleep(1.0)

            if exists("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job_id)):
                print_debug(get("//sys/scheduler/orchid/scheduler/jobs/{0}/brief_statistics".format(job_id)))

        assert suspicious


##################################################################


class TestMinNeededResources(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {"scheduler": {"min_needed_resources_update_period": 200}}

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "safe_scheduler_online_time": 500,
        }
    }

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {
                    "memory": 10 * 1024 * 1024 * 1024,
                    "cpu": 3,
                    "user_slots": 2,
                }
            }
        },
        "resource_limits": {"memory": 20 * 1024 * 1024 * 1024},
    }

    DELTA_MASTER_CONFIG = {"cypress_manager": {"default_table_replication_factor": 1}}

    @authors("ignat")
    def test_min_needed_resources(self):
        create("table", "//tmp/t_in")
        write_table("//tmp/t_in", [{"x": 1}])
        create("table", "//tmp/t_out")

        op1 = map(
            command="sleep 100; cat",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={
                "mapper": {
                    "memory_limit": 8 * 1024 * 1024 * 1024,
                    "memory_reserve_factor": 1.0,
                    "cpu_limit": 2,
                }
            },
            track=False,
        )

        op1_path = "//sys/scheduler/orchid/scheduler/operations/" + op1.id
        wait(lambda: exists(op1_path) and get(op1_path + "/state") == "running")

        time.sleep(3.0)

        assert get(op1.get_path() + "/controller_orchid/progress/schedule_job_statistics/count") > 0

        create("table", "//tmp/t2_in")
        write_table("//tmp/t2_in", [{"x": 1}])
        create("table", "//tmp/t2_out")

        op2 = map(
            command="cat",
            in_="//tmp/t2_in",
            out="//tmp/t2_out",
            spec={
                "mapper": {
                    "memory_limit": 3 * 1024 * 1024 * 1024,
                    "memory_reserve_factor": 1.0,
                    "cpu_limit": 2,
                }
            },
            track=False,
        )

        op2_path = "//sys/scheduler/orchid/scheduler/operations/" + op2.id
        wait(lambda: exists(op2_path) and get(op2_path + "/state") == "running")

        time.sleep(3.0)
        assert get(op2.get_path() + "/controller_orchid/progress/schedule_job_statistics/count") == 0

        abort_op(op1.id)

        op2.track()

    @authors("eshcherbin")
    def test_min_needed_resources_unsatisfied_count(self):
        op = run_sleeping_vanilla(task_patch={"cpu_limit": 2}, job_count=2)
        wait(
            lambda: get(
                scheduler_orchid_operation_path(op.id) + "/min_needed_resources_unsatisfied_count/cpu",
                default=0,
            )
            >= 10
        )


##################################################################


class TestSchedulingSegments(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 10
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "fair_share_update_period": 100,
            "fair_share_profiling_period": 100,
            "scheduling_segments_manage_period": 100,
        }
    }

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {
                    "cpu": 10,
                    "user_slots": 10,
                },
                "gpu_manager": {"test_resource": True, "test_gpu_count": 8},
            },
        },
        "scheduler_connector": {
            "heartbeat_period": 100,
        },
        "job_proxy_heartbeat_period": 100,
    }

    def _get_usage_ratio(self, op, tree="default"):
        return get(scheduler_orchid_operation_path(op, tree) + "/usage_ratio", default=0.0)

    def _get_fair_share_ratio(self, op, tree="default"):
        return get(scheduler_orchid_operation_path(op, tree) + "/fair_share_ratio", default=0.0)

    @classmethod
    def setup_class(cls):
        if is_asan_build():
            pytest.skip("test suite has too high memory consumption for ASAN build")
        super(TestSchedulingSegments, cls).setup_class()

    def setup_method(self, method):
        super(TestSchedulingSegments, self).setup_method(method)
        # NB(eshcherbin): This is done to reset node segments.
        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass
        create_pool("cpu", wait_for_orchid=False)
        create_pool("small_gpu", wait_for_orchid=False)
        create_pool("large_gpu")
        set("//sys/pool_trees/default/@config/scheduling_segments", {
            "mode": "large_gpu",
            "unsatisfied_segments_rebalancing_timeout": 1000,
        })
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/mode") == "large_gpu")
        wait(
            lambda: get(
                scheduler_orchid_default_pool_tree_config_path()
                + "/scheduling_segments/unsatisfied_segments_rebalancing_timeout"
            )
            == 1000
        )
        # Not to let preemption abort the jobs instead of segments manager.
        set("//sys/pool_trees/default/@config/preemptive_scheduling_backoff", 0)
        set("//sys/pool_trees/default/@config/fair_share_preemption_timeout", 100)
        set("//sys/pool_trees/default/@config/fair_share_preemption_timeout_limit", 100)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance", 0.95)
        set("//sys/pool_trees/default/@config/fair_share_starvation_tolerance_limit", 0.95)
        set("//sys/pool_trees/default/@config/max_unpreemptable_running_job_count", 80)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/max_unpreemptable_running_job_count") == 80)

    @authors("eshcherbin")
    def test_large_gpu_segment_extended(self):
        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @authors("eshcherbin")
    def test_default_segment_extended_gpu(self):
        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @pytest.mark.skip("There is no logic that reduces oversatisfied segments yet, "
                      "and operations with zero GPU demand do not change the default segment's fair resource amount")
    @authors("eshcherbin")
    def test_default_segment_extended_cpu(self):
        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(job_count=10, spec={"pool": "cpu"}, task_patch={"cpu_limit": 1})
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @pytest.mark.skip("There is no logic that reduces oversatisfied segments yet, "
                      "and operations with zero GPU demand do not change the default segment's fair resource amount")
    @authors("eshcherbin")
    def test_default_segment_extended_gpu_and_cpu(self):
        blocking_op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op1 = run_sleeping_vanilla(job_count=10, spec={"pool": "cpu"}, task_patch={"cpu_limit": 1})
        op2 = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )

        wait(lambda: are_almost_equal(self._get_usage_ratio(op1.id), 0.1))
        wait(lambda: are_almost_equal(self._get_usage_ratio(op2.id), 0.1))

    @authors("eshcherbin")
    def test_satisfaction_margins(self):
        fair_resource_amount_last = Metric.at_scheduler(
            "scheduler/segments/fair_resource_amount",
            grouped_by_tags=["segment"],
            aggr_method="last",
        )
        current_resource_amount_last = Metric.at_scheduler(
            "scheduler/segments/current_resource_amount",
            grouped_by_tags=["segment"],
            aggr_method="last",
        )

        blocking_op = run_sleeping_vanilla(
            job_count=4,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.05))

        # We have to leave one node for the default segment, otherwise it isn't satisfied.
        op = run_sleeping_vanilla(
            job_count=10,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.95))
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.9))

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 4)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 76)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 8)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 72)

        # Tweak satisfaction margin, but still fair resource of the default segment is positive, so nothing happens.
        set("//sys/pool_trees/default/@config/scheduling_segments/satisfaction_margins", {"default": -3.0})

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 1)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 76)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 8)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 72)

        # Finally, change satisfaction margin to be able to satisfy the large gpu segment.
        set("//sys/pool_trees/default/@config/scheduling_segments/satisfaction_margins", {"default": -4.0})

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 0)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 76)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 0)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 80)

        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 0.0))
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(blocking_op.id), 0.05))
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 1.0))
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.95))

    @authors("eshcherbin")
    def test_rebalancing_heuristic(self):
        blocking_op1 = run_sleeping_vanilla(
            job_count=9,
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op1.id), 0.9))

        # Need to spend some time to ensure the nodes where blocking_op1's jobs are running won't be moved.
        time.sleep(1.0)

        blocking_op2 = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op2.id), 0.1))

        def get_first_job_node(op):
            wait(lambda: len(op.get_running_jobs()) >= 1)
            jobs = op.get_running_jobs()
            job = jobs[list(jobs)[0]]
            return job["address"]

        expected_node = get_first_job_node(blocking_op2)
        wait(
            lambda: get(
                scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(expected_node),
                default="",
            )
            == "large_gpu"
        )

        new_op = run_sleeping_vanilla(
            job_count=8,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(new_op.id), 0.1))

        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op2.id), 0.0))
        actual_node = get_first_job_node(new_op)
        assert actual_node == expected_node
        wait(
            lambda: get(
                scheduler_orchid_path() + "/scheduler/nodes/{}/scheduling_segment".format(actual_node),
                default="",
            )
            == "default"
        )

    @authors("eshcherbin")
    def test_mixed_operation(self):
        op = vanilla(
            spec={
                "tasks": {
                    "small": {
                        "job_count": 1,
                        "command": "sleep 1000",
                        "gpu_limit": 1,
                        "enable_gpu_layers": False,
                    },
                    "large": {
                        "job_count": 1,
                        "command": "sleep 1000",
                        "gpu_limit": 8,
                        "enable_gpu_layers": False,
                    },
                }
            },
            track=False,
        )

        wait(
            lambda: get(
                scheduler_orchid_operation_path(op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

    @authors("eshcherbin")
    def test_specified_segment(self):
        small_but_large_op = run_sleeping_vanilla(
            spec={"pool": "small_gpu", "scheduling_segment": "large_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_but_small_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu", "scheduling_segment": "default"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )

        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_but_large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_but_small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

        with pytest.raises(YtError):
            run_sleeping_vanilla(
                spec={
                    "pool": "small_gpu",
                    "scheduling_segment": "my_cool_but_totally_invalid_segment",
                },
                task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
            )

    @authors("eshcherbin")
    def test_disabled(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "disabled")
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/mode") == "disabled")

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))

        op_slot_index_path = (
            scheduler_orchid_path() + "/scheduler/operations/{}/slot_index_per_pool_tree/default".format(op.id)
        )
        wait(lambda: exists(op_slot_index_path))
        op_slot_index = get(op_slot_index_path)
        op_usage_ratio_max = Metric.at_scheduler(
            "scheduler/operations_by_slot/usage_ratio_x100000",
            with_tags={"pool": "large_gpu", "slot_index": str(op_slot_index)},
            aggr_method="max",
        )

        time.sleep(3.0)
        wait(lambda: op_usage_ratio_max.update().get(verbose=True) == 0)

    @authors("eshcherbin")
    def test_rebalancing_timeout_changed(self):
        timeout_attribute_path = "/scheduling_segments/unsatisfied_segments_rebalancing_timeout"
        set("//sys/pool_trees/default/@config" + timeout_attribute_path, 1000000000)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + timeout_attribute_path) == 1000000000)

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))

        op_slot_index_path = (
            scheduler_orchid_path() + "/scheduler/operations/{}/slot_index_per_pool_tree/default".format(op.id)
        )
        wait(lambda: exists(op_slot_index_path))
        op_slot_index = get(op_slot_index_path)
        op_usage_ratio_max = Metric.at_scheduler(
            "scheduler/operations_by_slot/usage_ratio_x100000",
            with_tags={"pool": "large_gpu", "slot_index": str(op_slot_index)},
            aggr_method="max",
        )

        time.sleep(3.0)
        wait(lambda: op_usage_ratio_max.update().get(verbose=True) == 0)

        set("//sys/pool_trees/default/@config" + timeout_attribute_path, 1000)
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @authors("eshcherbin")
    def test_orchid(self):
        small_op = run_sleeping_vanilla(
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )

        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )

    @authors("eshcherbin")
    def test_update_operation_segment_on_reconfiguration(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "disabled")
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/mode") == "disabled")

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))
        wait(
            lambda: get(
                scheduler_orchid_operation_path(op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "large_gpu")
        wait(lambda: get(scheduler_orchid_operation_path(op.id) + "/scheduling_segment", default="") == "large_gpu")
        wait(lambda: get(scheduler_orchid_operation_path(blocking_op.id) + "/scheduling_segment", default="") == "default")
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

        set("//sys/pool_trees/default/@config/scheduling_segments/mode", "disabled")
        wait(lambda: get(scheduler_orchid_operation_path(op.id) + "/scheduling_segment", default="") == "default")
        wait(lambda: get(scheduler_orchid_operation_path(blocking_op.id) + "/scheduling_segment", default="") == "default")

        time.sleep(3.0)
        wait(lambda: are_almost_equal(self._get_usage_ratio(op.id), 0.1))

    @authors("eshcherbin")
    def test_profiling(self):
        set("//sys/pool_trees/default/@config/scheduling_segments/unsatisfied_segments_rebalancing_timeout", 1000000000)
        wait(lambda: get(scheduler_orchid_default_pool_tree_config_path() + "/scheduling_segments/unsatisfied_segments_rebalancing_timeout") == 1000000000)

        fair_resource_amount_last = Metric.at_scheduler(
            "scheduler/segments/fair_resource_amount",
            grouped_by_tags=["segment"],
            aggr_method="last",
        )
        current_resource_amount_last = Metric.at_scheduler(
            "scheduler/segments/current_resource_amount",
            grouped_by_tags=["segment"],
            aggr_method="last",
        )

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 0)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 0)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 80)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 0)

        blocking_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(blocking_op.id), 1.0))

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 80)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 0)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 80)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 0)

        op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_fair_share_ratio(op.id), 0.1))

        time.sleep(3.0)

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 72)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 8)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 80)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 0)

        set("//sys/pool_trees/default/@config/scheduling_segments/unsatisfied_segments_rebalancing_timeout", 1000)

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 72)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 8)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 72)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 8)

        op.abort()

        wait(lambda: fair_resource_amount_last.update().get("default", verbose=True) == 80)
        wait(lambda: fair_resource_amount_last.update().get("large_gpu", verbose=True) == 0)
        wait(lambda: current_resource_amount_last.update().get("default", verbose=True) == 80)
        wait(lambda: current_resource_amount_last.update().get("large_gpu", verbose=True) == 0)

    @authors("eshcherbin")
    def test_revive_operation_segments_from_scratch(self):
        small_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )

        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))

    @authors("eshcherbin")
    @pytest.mark.parametrize("service_to_restart", [SCHEDULERS_SERVICE, CONTROLLER_AGENTS_SERVICE])
    def test_revive_operation_segments_from_snapshot(self, service_to_restart):
        update_controller_agent_config("snapshot_period", 300)

        small_op = run_sleeping_vanilla(
            job_count=80,
            spec={"pool": "small_gpu"},
            task_patch={"gpu_limit": 1, "enable_gpu_layers": False},
        )
        large_op = run_sleeping_vanilla(
            spec={"pool": "large_gpu"},
            task_patch={"gpu_limit": 8, "enable_gpu_layers": False},
        )
        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))

        small_op.wait_for_fresh_snapshot()
        large_op.wait_for_fresh_snapshot()

        wait(lambda: exists(small_op.get_path() + "/@initial_aggregated_min_needed_resources"))
        wait(lambda: exists(large_op.get_path() + "/@initial_aggregated_min_needed_resources"))

        with Restarter(self.Env, service_to_restart):
            print_debug(get(small_op.get_path() + "/@initial_aggregated_min_needed_resources"))
            print_debug(get(large_op.get_path() + "/@initial_aggregated_min_needed_resources"))

        small_op.ensure_running()
        large_op.ensure_running()

        wait(
            lambda: get(
                scheduler_orchid_operation_path(large_op.id) + "/scheduling_segment",
                default="",
            )
            == "large_gpu"
        )
        wait(
            lambda: get(
                scheduler_orchid_operation_path(small_op.id) + "/scheduling_segment",
                default="",
            )
            == "default"
        )

        wait(lambda: are_almost_equal(self._get_usage_ratio(small_op.id), 0.9))
        wait(lambda: are_almost_equal(self._get_usage_ratio(large_op.id), 0.1))


##################################################################


class TestSchedulerInferChildrenWeightsFromHistoricUsage(YTEnvSetup):
    NUM_CPUS_PER_NODE = 10
    NUM_SLOTS_PER_NODE = 10

    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "fair_share_profiling_period": 100,
        }
    }

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {
                    "cpu": NUM_CPUS_PER_NODE,
                    "user_slots": NUM_SLOTS_PER_NODE,
                }
            }
        }
    }

    def setup_method(self, method):
        super(TestSchedulerInferChildrenWeightsFromHistoricUsage, self).setup_method(method)
        create_pool("parent")
        set("//sys/pools/parent/@infer_children_weights_from_historic_usage", True)
        set(
            "//sys/pools/parent/@historic_usage_config",
            {"aggregation_mode": "exponential_moving_average", "ema_alpha": 1.0},
        )
        time.sleep(0.5)

    def _init_children(self, num_children=2):
        for i in xrange(num_children):
            create_pool("child" + str(i + 1), parent_name="parent")

    def _get_pool_fair_share_ratio(self, pool):
        try:
            return get(scheduler_orchid_pool_path(pool) + "/fair_share_ratio")
        except YtError:
            return 0.0

    def _get_pool_usage_ratio(self, pool):
        try:
            return get(scheduler_orchid_pool_path(pool) + "/usage_ratio")
        except YtError:
            return 0.0

    def _get_pool_profiling(self, sensor_name, start_time, reduce="none"):
        assert reduce in ("none", "last", "max")
        result = {}
        for entry in get(
            "//sys/scheduler/orchid/profiling/scheduler/pools/" + sensor_name,
            from_time=int(start_time * 1000000),
            verbose=False,
        ):
            pool = entry["tags"]["pool"]
            if pool in result:
                result[pool].append(entry["value"])
            else:
                result[pool] = [entry["value"]]
        for pool in result:
            if reduce == "last":
                result[pool] = result[pool][-1]
            elif reduce == "max":
                result[pool] = max(result[pool])
        print_debug("Pool profiling (reduce='{}'):".format(reduce), result)
        return result

    def _test_more_fair_share_for_new_operation_base(self, num_jobs_op1, num_jobs_op2):
        self._init_children()

        op1_tasks_spec = {"task": {"job_count": num_jobs_op1, "command": "sleep 100;"}}
        op1 = vanilla(spec={"pool": "child1", "tasks": op1_tasks_spec}, track=False)

        wait(
            lambda: are_almost_equal(
                self._get_pool_usage_ratio("child1"),
                min(num_jobs_op1 / self.NUM_SLOTS_PER_NODE, 1.0),
            )
        )

        # give some time for historic usage to accumulate
        time.sleep(2)

        op2_tasks_spec = {"task": {"job_count": num_jobs_op2, "command": "sleep 100;"}}

        fair_share_ratio_max = Metric.at_scheduler(
            "scheduler/pools/fair_share_ratio_x100000",
            with_tags={"pool": "child2"},
            aggr_method="max",
        )

        op2 = vanilla(spec={"pool": "child2", "tasks": op2_tasks_spec}, track=False)

        # it's hard to estimate historic usage for all children, because run time can vary and jobs
        # can spuriously abort and restart; so we don't set the threshold any greater than 0.5
        wait(lambda: fair_share_ratio_max.update().get(verbose=True) is not None)
        wait(lambda: fair_share_ratio_max.update().get(verbose=True) > 0.5 * 100000)

        op1.complete()
        op2.complete()

    @authors("eshcherbin")
    def test_more_fair_share_for_new_operation_equal_demand(self):
        self._test_more_fair_share_for_new_operation_base(10, 10)

    @authors("eshcherbin")
    def test_more_fair_share_for_new_operation_bigger_demand(self):
        self._test_more_fair_share_for_new_operation_base(5, 10)

    @authors("eshcherbin")
    def test_more_fair_share_for_new_operation_smaller_demand(self):
        self._test_more_fair_share_for_new_operation_base(10, 6)

    # NB(eshcherbin): this test works only if new config effectively disables historic usage aggregation
    def _test_equal_fair_share_after_disabling_config_change_base(self, new_config):
        self._init_children()

        op1_tasks_spec = {"task": {"job_count": self.NUM_SLOTS_PER_NODE, "command": "sleep 100;"}}
        op1 = vanilla(spec={"pool": "child1", "tasks": op1_tasks_spec}, track=False)

        wait(lambda: are_almost_equal(self._get_pool_usage_ratio("child1"), 1.0))

        # give some time for historic usage to accumulate
        time.sleep(2)

        # change config and wait for it to be applied
        set(
            "//sys/pools/parent/@infer_children_weights_from_historic_usage",
            new_config["infer_children_weights_from_historic_usage"],
        )
        wait(
            lambda: get("//sys/pools/parent/@infer_children_weights_from_historic_usage")
            == new_config["infer_children_weights_from_historic_usage"]
        )
        set(
            "//sys/pools/parent/@historic_usage_config",
            new_config["historic_usage_config"],
        )
        wait(lambda: get("//sys/pools/parent/@historic_usage_config") == new_config["historic_usage_config"])
        time.sleep(0.5)

        op2_tasks_spec = {"task": {"job_count": self.NUM_SLOTS_PER_NODE, "command": "sleep 100;"}}

        fair_share_ratio_max = Metric.at_scheduler(
            "scheduler/pools/fair_share_ratio_x100000",
            with_tags={"pool": "child2"},
            aggr_method="max",
        )

        op2 = vanilla(spec={"pool": "child2", "tasks": op2_tasks_spec}, track=False)

        wait(lambda: fair_share_ratio_max.update().get(verbose=True) is not None)
        wait(lambda: fair_share_ratio_max.update().get(verbose=True) in (49999, 50000, 50001))

        op1.complete()
        op2.complete()

    @authors("eshcherbin")
    def test_equal_fair_share_after_disabling_historic_usage(self):
        self._test_equal_fair_share_after_disabling_config_change_base(
            {
                "infer_children_weights_from_historic_usage": False,
                "historic_usage_config": {"aggregation_mode": "none", "ema_alpha": 0.0},
            }
        )

    @authors("eshcherbin")
    def test_equal_fair_share_after_disabling_historic_usage_but_keeping_parameters(
        self,
    ):
        self._test_equal_fair_share_after_disabling_config_change_base(
            {
                "infer_children_weights_from_historic_usage": False,
                "historic_usage_config": {
                    "aggregation_mode": "exponential_moving_average",
                    "ema_alpha": 1.0,
                },
            }
        )

    @authors("eshcherbin")
    def test_equal_fair_share_after_setting_none_mode(self):
        self._test_equal_fair_share_after_disabling_config_change_base(
            {
                "infer_children_weights_from_historic_usage": True,
                "historic_usage_config": {"aggregation_mode": "none", "ema_alpha": 1.0},
            }
        )

    @authors("eshcherbin")
    def test_equal_fair_share_after_setting_zero_alpha(self):
        self._test_equal_fair_share_after_disabling_config_change_base(
            {
                "infer_children_weights_from_historic_usage": True,
                "historic_usage_config": {
                    "aggregation_mode": "exponential_moving_average",
                    "ema_alpha": 0.0,
                },
            }
        )


##################################################################


@authors("renadeen")
class TestIntegralGuarantees(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,  # Update pools configuration period
        }
    }

    DELTA_NODE_CONFIG = {"exec_agent": {"job_controller": {"resource_limits": {"cpu": 10, "user_slots": 10}}}}

    def wait_pool_fair_share(self, pool, min_share, integral, weight_proportional):
        path = scheduler_orchid_default_pool_tree_path() + "/pools/" + pool + "/detailed_fair_share"
        wait(lambda: exists(path))

        def check_pool_fair_share():
            fair_share = get(path, default=-1)
            return (
                are_almost_equal(fair_share["min_share_guarantee"]["cpu"], min_share)
                and are_almost_equal(fair_share["integral_guarantee"]["cpu"], integral)
                and are_almost_equal(fair_share["weight_proportional"]["cpu"], weight_proportional)
            )

        wait(check_pool_fair_share)

    def setup_method(self, method):
        super(TestIntegralGuarantees, self).setup_method(method)
        set("//sys/pool_trees/default/@config/integral_guarantees", {"smooth_period": 500})

    def test_simple_burst_integral_guarantee(self):
        create_pool(
            "burst_pool",
            attributes={
                "min_share_resources": {"cpu": 2},
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 3},
                    "burst_guarantee_resources": {"cpu": 8},
                },
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "burst_pool"})
        self.wait_pool_fair_share("burst_pool", min_share=0.2, integral=0.3, weight_proportional=0.5)

    def test_simple_relaxed_integral_guarantee(self):
        create_pool(
            "relaxed_pool",
            attributes={
                "min_share_resources": {"cpu": 2},
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 3},
                },
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "relaxed_pool"})
        self.wait_pool_fair_share("relaxed_pool", min_share=0.2, integral=0.3, weight_proportional=0.5)

    def test_min_share_vs_burst(self):
        create_pool(
            "min_share_pool",
            attributes={
                "min_share_resources": {"cpu": 5},
            },
        )

        create_pool(
            "burst_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 5},
                    "burst_guarantee_resources": {"cpu": 5},
                }
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "min_share_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "burst_pool"})

        self.wait_pool_fair_share("min_share_pool", min_share=0.5, integral=0.0, weight_proportional=0.0)

        self.wait_pool_fair_share("burst_pool", min_share=0.0, integral=0.5, weight_proportional=0.0)

        self.wait_pool_fair_share("<Root>", min_share=0.5, integral=0.5, weight_proportional=0.0)

    def test_min_share_vs_relaxed(self):
        create_pool(
            "min_share_pool",
            attributes={
                "min_share_resources": {"cpu": 5},
            },
        )

        create_pool(
            "relaxed_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 5},
                }
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "min_share_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "relaxed_pool"})

        self.wait_pool_fair_share("min_share_pool", min_share=0.5, integral=0.0, weight_proportional=0.0)

        self.wait_pool_fair_share("relaxed_pool", min_share=0.0, integral=0.5, weight_proportional=0.0)

        self.wait_pool_fair_share("<Root>", min_share=0.5, integral=0.5, weight_proportional=0.0)

    def test_relaxed_vs_empty_min_share(self):
        create_pool(
            "min_share_pool",
            attributes={
                "min_share_resources": {"cpu": 5},
            },
        )

        create_pool(
            "relaxed_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 10},
                }
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "relaxed_pool"})

        self.wait_pool_fair_share("min_share_pool", min_share=0.0, integral=0.0, weight_proportional=0.0)

        self.wait_pool_fair_share("relaxed_pool", min_share=0.0, integral=1.0, weight_proportional=0.0)

        self.wait_pool_fair_share("<Root>", min_share=0.0, integral=1.0, weight_proportional=0.0)

    def test_relaxed_vs_no_guarantee(self):
        create_pool(
            "relaxed_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 10},
                }
            },
        )

        create_pool("no_guarantee_pool")

        run_sleeping_vanilla(job_count=10, spec={"pool": "relaxed_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "no_guarantee_pool"})

        self.wait_pool_fair_share("relaxed_pool", min_share=0.0, integral=1.0, weight_proportional=0.0)

        self.wait_pool_fair_share("no_guarantee_pool", min_share=0.0, integral=0.0, weight_proportional=0.0)

        self.wait_pool_fair_share("<Root>", min_share=0.0, integral=1.0, weight_proportional=0.0)

    def test_burst_gets_all_relaxed_none(self):
        create_pool(
            "burst_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 10},
                    "burst_guarantee_resources": {"cpu": 10},
                }
            },
        )

        create_pool(
            "relaxed_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 5},
                }
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "burst_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "relaxed_pool"})

        self.wait_pool_fair_share("burst_pool", min_share=0.0, integral=1.0, weight_proportional=0.0)

        self.wait_pool_fair_share("relaxed_pool", min_share=0.0, integral=0.0, weight_proportional=0.0)

        self.wait_pool_fair_share("<Root>", min_share=0.0, integral=1.0, weight_proportional=0.0)

    def test_burst_gets_its_guarantee_relaxed_gets_remaining(self):
        create_pool(
            "burst_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 6},
                    "burst_guarantee_resources": {"cpu": 6},
                }
            },
        )

        create_pool(
            "relaxed_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 10},
                }
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "burst_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "relaxed_pool"})

        self.wait_pool_fair_share("burst_pool", min_share=0.0, integral=0.6, weight_proportional=0.0)

        self.wait_pool_fair_share("relaxed_pool", min_share=0.0, integral=0.4, weight_proportional=0.0)

        self.wait_pool_fair_share("<Root>", min_share=0.0, integral=1.0, weight_proportional=0.0)

    def test_all_kinds_of_pools_weight_proportional_distribution(self):
        create_pool(
            "min_share_pool",
            attributes={"min_share_resources": {"cpu": 1}, "weight": 3},
        )

        create_pool(
            "burst_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 1},
                    "burst_guarantee_resources": {"cpu": 1},
                },
                "weight": 1,
            },
        )

        create_pool(
            "relaxed_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 1},
                },
                "weight": 1,
            },
        )

        create_pool("no_guarantee_pool", attributes={"weight": 2})

        run_sleeping_vanilla(job_count=10, spec={"pool": "min_share_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "burst_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "relaxed_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "no_guarantee_pool"})

        self.wait_pool_fair_share("min_share_pool", min_share=0.1, integral=0.0, weight_proportional=0.3)

        self.wait_pool_fair_share("burst_pool", min_share=0.0, integral=0.1, weight_proportional=0.1)

        self.wait_pool_fair_share("relaxed_pool", min_share=0.0, integral=0.1, weight_proportional=0.1)

        self.wait_pool_fair_share("no_guarantee_pool", min_share=0.0, integral=0.0, weight_proportional=0.2)

        self.wait_pool_fair_share("<Root>", min_share=0.1, integral=0.2, weight_proportional=0.7)

    def test_min_share_and_burst_guarantees_adjustment(self):
        create_pool(
            "min_share_pool",
            attributes={
                "min_share_resources": {"cpu": 6},
            },
        )

        create_pool(
            "burst_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 9},
                    "burst_guarantee_resources": {"cpu": 9},
                }
            },
        )

        run_sleeping_vanilla(job_count=10, spec={"pool": "min_share_pool"})
        run_sleeping_vanilla(job_count=10, spec={"pool": "burst_pool"})

        self.wait_pool_fair_share("min_share_pool", min_share=0.4, integral=0.0, weight_proportional=0.0)

        self.wait_pool_fair_share("burst_pool", min_share=0.0, integral=0.6, weight_proportional=0.0)

        self.wait_pool_fair_share("<Root>", min_share=0.4, integral=0.6, weight_proportional=0.0)

    def test_accumulated_resource_ratio_volume_consumption(self):
        create_pool(
            "test_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 5},
                }
            },
        )
        wait(lambda: exists(scheduler_orchid_default_pool_tree_path() + "/pools/test_pool"))

        # No operations -> volume is accumulating
        wait(
            lambda: get(
                scheduler_orchid_default_pool_tree_path() + "/pools/test_pool/accumulated_resource_ratio_volume"
            )
            > 1
        )
        run_sleeping_vanilla(job_count=10, spec={"pool": "test_pool"})

        # Minimum volume is 0.25 = 0.5 (period) * 0.5 (flow_ratio)
        wait(
            lambda: get(
                scheduler_orchid_default_pool_tree_path() + "/pools/test_pool/accumulated_resource_ratio_volume"
            )
            < 0.3
        )

    def test_accumulated_resource_ratio_volume_survives_restart(self):
        create_pool(
            "test_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 10},
                }
            },
        )

        wait(lambda: exists(scheduler_orchid_pool_path("test_pool")))
        wait(lambda: get(scheduler_orchid_pool_path("test_pool") + "/accumulated_resource_volume/cpu") > 30)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        wait(lambda: exists(scheduler_orchid_pool_path("test_pool")), sleep_backoff=0.1)
        # Wait for total resource limits to appear.
        wait(
            lambda: get(scheduler_orchid_pool_path("<Root>") + "/resource_limits/cpu") > 0,
            sleep_backoff=0.1,
        )

        volume = get(scheduler_orchid_pool_path("test_pool") + "/accumulated_resource_volume/cpu")
        assert 30 < volume < 50

    def test_integral_pools_orchid(self):
        create_pool(
            "burst_pool",
            attributes={
                "min_share_resources": {"cpu": 2},
                "integral_guarantees": {
                    "guarantee_type": "burst",
                    "resource_flow": {"cpu": 3},
                    "burst_guarantee_resources": {"cpu": 7},
                },
            },
        )

        create_pool(
            "relaxed_pool",
            attributes={
                "integral_guarantees": {
                    "guarantee_type": "relaxed",
                    "resource_flow": {"cpu": 5},
                }
            },
        )

        root_pool_path = scheduler_orchid_default_pool_tree_path() + "/pools/<Root>"
        burst_pool_path = scheduler_orchid_default_pool_tree_path() + "/pools/burst_pool"
        relaxed_pool_path = scheduler_orchid_default_pool_tree_path() + "/pools/relaxed_pool"
        wait(lambda: exists(relaxed_pool_path))

        assert get(burst_pool_path + "/integral_guarantee_type") == "burst"
        assert get(burst_pool_path + "/specified_burst_ratio") == 0.7
        assert get(burst_pool_path + "/specified_resource_flow_ratio") == 0.3
        assert get(burst_pool_path + "/integral_pool_capacity/cpu") == 3.0 * 86400

        assert get(relaxed_pool_path + "/integral_guarantee_type") == "relaxed"
        assert get(relaxed_pool_path + "/specified_resource_flow_ratio") == 0.5
        assert get(relaxed_pool_path + "/integral_pool_capacity/cpu") == 5.0 * 86400

        assert get(root_pool_path + "/total_resource_flow_ratio") == 0.8
        assert get(root_pool_path + "/total_burst_ratio") == 0.7


##################################################################


class TestSatisfactionRatio(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,  # Update pools configuration period
        }
    }

    @authors("eshcherbin")
    def test_satisfaction_simple(self):
        create_pool("pool")

        op1 = run_test_vanilla(with_breakpoint("BREAKPOINT"), spec={"pool": "pool"}, job_count=2)
        wait_breakpoint(job_count=1)

        op2 = run_sleeping_vanilla(spec={"pool": "pool"})

        wait(
            lambda: are_almost_equal(
                get(scheduler_orchid_operation_path(op1.id) + "/satisfaction_ratio"),
                2.0,
            )
        )
        wait(
            lambda: are_almost_equal(
                get(scheduler_orchid_operation_path(op2.id) + "/satisfaction_ratio"),
                0.0,
            )
        )
        wait(lambda: are_almost_equal(get(scheduler_orchid_pool_path("pool") + "/satisfaction_ratio"), 0.0))

        release_breakpoint()

        op1.wait_for_state("completed")

        wait(
            lambda: are_almost_equal(
                get(scheduler_orchid_operation_path(op2.id) + "/satisfaction_ratio"),
                1.0,
            )
        )
        wait(lambda: are_almost_equal(get(scheduler_orchid_pool_path("pool") + "/satisfaction_ratio"), 1.0))
