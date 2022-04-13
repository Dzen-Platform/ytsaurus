from yt_env_setup import YTEnvSetup, Restarter, NODES_SERVICE

from yt_commands import (
    authors, create, map, wait, with_breakpoint, wait_breakpoint,
    write_table, update_controller_agent_config, update_nodes_dynamic_config)

from yt_helpers import JobCountProfiler

##################################################################


class TestNodeHeartbeats(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_NODE_CONFIG = {
        "exec_agent": {
            "scheduler_connector": {"heartbeat_period": 100},
            "controller_agent_connector": {"heartbeat_period": 100},
        },
        "dynamic_config_manager": {
            "update_period": 100,
        },
    }

    def _test(self, fixup):
        update_controller_agent_config("full_job_info_wait_timeout", 1000)
        update_nodes_dynamic_config({
            "exec_agent": {
                "controller_agent_connector": {
                    "test_heartbeat_delay": 3000,
                }
            }
        })

        create("table", "//tmp/in")
        create("table", "//tmp/out")
        for _ in range(5):
            write_table("<append=true>//tmp/in", {"foo": "bar"})

        command = "cat;"
        op = map(
            track=False,
            in_="//tmp/in",
            out="//tmp/out",
            command=command,
            spec={"data_size_per_job": 1},
        )

        wait(lambda: op.get_job_count("aborted") >= 3)
        assert op.get_state() == "running"

        fixup()

        def check_operation_completed():
            return op.get_state() == "completed"
        wait(check_operation_completed)

        op.track()

    @authors("pogorelov")
    def test_job_abort_on_heartbeat_timeout(self):
        def fixup():
            update_nodes_dynamic_config({
                "exec_agent": {
                    "controller_agent_connector": {
                        "test_heartbeat_delay": 0,
                    }
                }
            })

        self._test(fixup)

    @authors("pogorelov")
    def test_abort_by_scheduler(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", {"foo": "bar"})

        op = map(
            track=False,
            in_="//tmp/in",
            out="//tmp/out",
            command=with_breakpoint("BREAKPOINT"),
            spec={"data_size_per_job": 1},
        )

        wait_breakpoint()

        aborted_job_profiler = JobCountProfiler("aborted", tags={"tree": "default", "job_type": "map"})

        with Restarter(self.Env, NODES_SERVICE):
            wait(lambda: op.get_job_count("aborted") == 1)
            wait(lambda: aborted_job_profiler.get_job_count_delta(tags={"abort_reason": "node_offline"}) == 1)
            assert aborted_job_profiler.get_job_count_delta(tags={"abort_reason": "job_statistics_wait_timeout"}) == 0
