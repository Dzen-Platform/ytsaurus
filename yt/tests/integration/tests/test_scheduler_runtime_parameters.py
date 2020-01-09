from yt_env_setup import YTEnvSetup, wait, Restarter, SCHEDULERS_SERVICE
from yt.test_helpers import are_almost_equal
from yt_commands import *

import pytest
import gzip


class TestRuntimeParameters(YTEnvSetup):

    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "operations_update_period": 10,
            "pool_change_is_allowed": True,
            "watchers_update_period": 100,  # Update pools configuration period
        }
    }

    @authors("renadeen")
    def test_update_runtime_parameters(self):
        create_test_tables()

        op = map(
            command="sleep 100",
            in_="//tmp/t_in",
            out="//tmp/t_out",
            spec={"weight": 5},
            track=False)
        wait(lambda: op.get_state() == "running", iter=10)

        progress_path = "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default".format(op.id)
        assert get(progress_path + "/weight") == 5.0

        update_op_parameters(op.id, parameters={
            "scheduling_options_per_pool_tree": {
                "default": {
                    "weight": 3.0,
                    "resource_limits": {
                        "user_slots": 0
                    }
                }
            }
        })

        default_tree_parameters_path = op.get_path() + "/@runtime_parameters/scheduling_options_per_pool_tree/default"

        wait(lambda: are_almost_equal(get(default_tree_parameters_path + "/weight"), 3.0))
        wait(lambda: get(default_tree_parameters_path + "/resource_limits/user_slots") == 0)

        wait(lambda: are_almost_equal(get(progress_path + "/weight"), 3.0))
        # wait() is essential since resource limits are copied from runtime parameters only during fair-share update.
        wait(lambda: get(progress_path + "/resource_limits")["user_slots"] == 0, iter=5)

        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass

        wait(lambda: op.get_state() == "running", iter=10)

        wait(lambda: are_almost_equal(get(progress_path + "/weight"), 3.0))
        # wait() is essential since resource limits are copied from runtime parameters only during fair-share update.
        wait(lambda: get(progress_path + "/resource_limits")["user_slots"] == 0, iter=5)

    @authors("renadeen")
    def test_change_pool_of_default_pooltree(self):
        create_pool("initial_pool")
        create_pool("changed_pool")

        op = run_sleeping_vanilla(spec={"pool": "initial_pool"})

        wait(lambda: op.get_state() == "running", iter=10)

        update_op_parameters(op.id, parameters={"pool": "changed_pool"})

        path = "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/pool".format(op.id)
        wait(lambda: get(path) == "changed_pool")

    @authors("renadeen", "ignat")
    def test_running_operation_counts_on_change_pool(self):
        pools_path = scheduler_orchid_default_pool_tree_path() + "/pools"

        create_pool("initial_pool")
        create_pool("changed_pool")
        wait(lambda: exists(pools_path + "/changed_pool"))

        op = run_sleeping_vanilla(spec={"pool": "initial_pool"})
        wait(lambda: op.get_state() == "running", iter=10)

        wait(lambda: get(pools_path + "/initial_pool/running_operation_count") == 1)
        wait(lambda: get(pools_path + "/changed_pool/running_operation_count") == 0)

        update_op_parameters(op.id, parameters={"pool": "changed_pool"})

        wait(lambda: get(pools_path + "/initial_pool/running_operation_count") == 0)
        wait(lambda: get(pools_path + "/changed_pool/running_operation_count") == 1)

    @authors("renadeen")
    def test_change_pool_of_multitree_operation(self):
        self.create_custom_pool_tree_with_one_node(pool_tree="custom")
        create_pool("default_pool")
        create_pool("custom_pool1", pool_tree="custom")
        create_pool("custom_pool2", pool_tree="custom")
        time.sleep(0.1)

        op = run_sleeping_vanilla(
            spec={
                "pool_trees": ["default", "custom"],
                "scheduling_options_per_pool_tree": {
                    "default": {"pool": "default_pool"},
                    "custom": {"pool": "custom_pool1"}
                }
            })

        wait(lambda: op.get_state() == "running", iter=10)

        update_op_parameters(op.id, parameters={"scheduling_options_per_pool_tree": {"custom": {"pool": "custom_pool2"}}})

        path = "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/custom/pool".format(op.id)
        wait(lambda: get(path) == "custom_pool2")

    @authors("renadeen")
    def test_operation_count_validation_on_change_pool(self):
        create_pool("initial_pool")
        create_pool("full_pool", attributes={"max_running_operation_count": 0})

        op = run_sleeping_vanilla(spec={"pool": "initial_pool"})
        wait(lambda: op.get_state() == "running")

        with pytest.raises(YtError):
            update_op_parameters(op.id, parameters={"pool": "full_pool"})

        path = "//sys/scheduler/orchid/scheduler/operations/{0}/progress/scheduling_info_per_pool_tree/default/pool".format(op.id)
        assert get(path) == "initial_pool"

    @authors("renadeen")
    def test_change_pool_during_prepare_phase_bug(self):
        op = run_test_vanilla(":", spec={"testing": {"delay_inside_prepare": 2000}})
        wait(lambda: op.get_state() == "preparing", sleep_backoff=0.1)
        update_op_parameters(op.id, parameters={"pool": "another_pool"})
        assert op.get_state() == "preparing"
        # YT-11311: core was in MaterializeOperation.
        op.track()

    @authors("renadeen")
    def test_no_pool_validation_on_change_weight(self):
        create_pool("test_pool")
        op = run_sleeping_vanilla(spec={"pool": "test_pool"})
        wait(lambda: op.get_state() == "running")

        set("//sys/pools/test_pool/@max_operation_count", 0)
        set("//sys/pools/test_pool/@max_running_operation_count", 0)

        orchid_pools = scheduler_orchid_default_pool_tree_path() + "/pools"
        wait(lambda: get(orchid_pools + "/test_pool/max_running_operation_count") == 0)

        # assert this doesn't fail
        update_op_parameters(op.id, parameters={"weight": 2})

    def create_custom_pool_tree_with_one_node(self, pool_tree):
        tag = pool_tree
        node = ls("//sys/cluster_nodes")[0]
        set("//sys/cluster_nodes/" + node + "/@user_tags/end", tag)
        create_pool_tree(pool_tree, attributes={"nodes_filter": tag})
        set("//sys/pool_trees/default/@nodes_filter", "!" + tag)
        return node


class TestJobsAreScheduledAfterPoolChange(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "fair_share_update_period": 100,
            "operations_update_period": 10,
            "pool_change_is_allowed": True
        }
    }

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "job_controller": {
                "resource_limits": {
                    "user_slots": 10,
                    "cpu": 10,
                    "memory": 10 * 1024 ** 3,
                }
            }
        }
    }

    @authors("renadeen", "antonkikh")
    def test_jobs_are_scheduled_after_pool_change(self):
        create_pool("initial_pool")
        create_pool("changed_pool")
        op = run_test_vanilla(":", job_count=100000, spec={"pool": "initial_pool"})
        wait(lambda: op.get_job_count("running") > 5, iter=10)

        update_op_parameters(op.id, parameters={"pool": "changed_pool"})
        time.sleep(0.1)

        scheduled = op.get_job_count("running") + op.get_job_count("completed")
        wait(lambda: op.get_job_count("running") + op.get_job_count("completed") > scheduled + 10)


class TestOperationDetailedLogs(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 10
    NUM_SCHEDULERS = 1

    def get_scheduled_job_log_entries(self):
        scheduler_debug_logs_filename = self.Env.configs["scheduler"][0]["logging"]["writers"]["debug"]["file_name"]

        if scheduler_debug_logs_filename.endswith(".gz"):
            logfile = gzip.open(scheduler_debug_logs_filename, "r")
        else:
            logfile = open(scheduler_debug_logs_filename, "r")
        
        return [line for line in logfile if "Scheduled a job" in line]

    @authors("antonkikh")
    def test_enable_detailed_logs(self):
        create_pool("fake_pool")
        set("//sys/pool_trees/default/fake_pool/@resource_limits", {"user_slots": 3})

        op = run_sleeping_vanilla(job_count=10, spec={"pool": "fake_pool"})
        wait(lambda: len(op.get_running_jobs()) == 3)

        # Check that there are no detailed logs by default.

        assert len(self.get_scheduled_job_log_entries()) == 0

        # Enable detailed logging and check that expected the expected log entries are produced.

        update_op_parameters(op.id, parameters={
            "scheduling_options_per_pool_tree": {
                "default": {
                    "enable_detailed_logs": True,
                }
            }
        })
        time.sleep(1)

        assert len(op.get_running_jobs()) == 3
        set("//sys/pool_trees/default/fake_pool/@resource_limits/user_slots", 5)
        wait(lambda: len(op.get_running_jobs()) == 5)
        time.sleep(0.5)  # Give it time to flush the log to disk.

        log_entries = self.get_scheduled_job_log_entries()
        assert len(log_entries) == 2
        for log_entry in log_entries:
            assert "OperationId: {}".format(op.id) in log_entry
            assert "TreeId: default" in log_entry

        # Disable detailed logging and check that no new log entries are produced.

        update_op_parameters(op.id, parameters={
            "scheduling_options_per_pool_tree": {
                "default": {
                    "enable_detailed_logs": False,
                }
            }
        })
        time.sleep(1)

        assert len(op.get_running_jobs()) == 5
        set("//sys/pool_trees/default/fake_pool/@resource_limits/user_slots", 7)
        wait(lambda: len(op.get_running_jobs()) == 7)
        time.sleep(0.5)  # Give it time to flush the log to disk.

        assert len(log_entries) == 2

        op.abort()

    @authors("antonkikh")
    def test_enable_detailed_logs_requires_administer_permission(self):
        create_user("u1")
        op = run_sleeping_vanilla(job_count=10, authenticated_user="u1")

        def update_enable_detailed_logs():
            update_op_parameters(
                op.id,
                parameters={
                    "scheduling_options_per_pool_tree": {
                        "default": {
                            "enable_detailed_logs": False,
                        }
                    },
                },
                authenticated_user="u1",
            )

        with pytest.raises(YtError) as excinfo:
            update_enable_detailed_logs()
        if not excinfo.value.contains_code(AuthorizationErrorCode):
            raise excinfo.value

        add_member("u1", "superusers")
        update_enable_detailed_logs()
