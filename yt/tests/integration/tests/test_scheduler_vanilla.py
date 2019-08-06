import pytest

from yt_env_setup import YTEnvSetup, wait, Restarter, SCHEDULERS_SERVICE
from yt_commands import *

from yt.yson import to_yson_type

import datetime

from collections import Counter

##################################################################

class TestSchedulerVanillaCommands(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "snapshot_period": 200,
        }
    }

    @authors("max42")
    def test_simple(self):
        command = " ; ".join([
            events_on_fs().notify_event_cmd("job_started_${YT_JOB_INDEX}"),
            events_on_fs().wait_event_cmd("finish")
        ])
        op = vanilla(
            dont_track=True,
            spec={
                "tasks": {
                    "master": {
                        "job_count": 1,
                        "command": command,
                    },
                    "slave": {
                        "job_count": 2,
                        "command": command,
                    },
                },
            })

        # Ensure that all three jobs have started.
        events_on_fs().wait_event("job_started_0", timeout=datetime.timedelta(1000))
        events_on_fs().wait_event("job_started_1", timeout=datetime.timedelta(1000))
        events_on_fs().wait_event("job_started_2", timeout=datetime.timedelta(1000))

        events_on_fs().notify_event("finish")

        op.track()

        data_flow_graph_path = op.get_path() + "/@progress/data_flow_graph"
        get(data_flow_graph_path)
        assert get(data_flow_graph_path + "/vertices/master/job_type") == "vanilla"
        assert get(data_flow_graph_path + "/vertices/master/job_counter/completed/total") == 1
        assert get(data_flow_graph_path + "/vertices/slave/job_type") ==  "vanilla"
        assert get(data_flow_graph_path + "/vertices/slave/job_counter/completed/total") == 2

    @authors("max42", "ignat")
    def test_task_job_index(self):
        master_command = " ; ".join([
            events_on_fs().notify_event_cmd("job_started_master_${YT_TASK_JOB_INDEX}"),
            events_on_fs().wait_event_cmd("finish")
        ])

        slave_command = " ; ".join([
            events_on_fs().notify_event_cmd("job_started_slave_${YT_TASK_JOB_INDEX}"),
            events_on_fs().wait_event_cmd("finish")
        ])

        op = vanilla(
            dont_track=True,
            spec={
                "tasks": {
                    "master": {
                        "job_count": 1,
                        "command": master_command,
                    },
                    "slave": {
                        "job_count": 3,
                        "command": slave_command,
                    },
                },
            })

        # Ensure that all three jobs have started.
        events_on_fs().wait_event("job_started_master_0", timeout=datetime.timedelta(1000))
        events_on_fs().wait_event("job_started_slave_0", timeout=datetime.timedelta(1000))
        events_on_fs().wait_event("job_started_slave_1", timeout=datetime.timedelta(1000))
        events_on_fs().wait_event("job_started_slave_2", timeout=datetime.timedelta(1000))

        events_on_fs().notify_event("finish")

        op.track()

        data_flow_graph_path = op.get_path() + "/@progress/data_flow_graph"
        get(data_flow_graph_path)
        assert get(data_flow_graph_path + "/vertices/master/job_type") == "vanilla"
        assert get(data_flow_graph_path + "/vertices/master/job_counter/completed/total") == 1
        assert get(data_flow_graph_path + "/vertices/slave/job_type") ==  "vanilla"
        assert get(data_flow_graph_path + "/vertices/slave/job_counter/completed/total") == 3

    @authors("max42")
    def test_files(self):
        create("file", "//tmp/a")
        write_file("//tmp/a", "data_a")
        create("file", "//tmp/b")
        write_file("//tmp/b", "data_b")

        vanilla(
            spec={
                "tasks": {
                    "task_a": {
                        "job_count": 1,
                        "command": 'if [[ `cat data` != "data_a" ]] ; then exit 1; fi',
                        "file_paths": [to_yson_type("//tmp/a", attributes={"file_name": "data"})]
                    },
                    "task_b": {
                        "job_count": 2,
                        "command": 'if [[ `cat data` != "data_b" ]] ; then exit 1; fi',
                        "file_paths": [to_yson_type("//tmp/b", attributes={"file_name": "data"})]
                    },
                },
                "max_failed_job_count": 1,
            })

    @authors("max42")
    def test_stderr(self):
        create("table", "//tmp/stderr")

        op = vanilla(
            spec={
                "tasks": {
                    "task_a": {
                        "job_count": 3,
                        "command": 'echo "task_a" >&2',
                    },
                    "task_b": {
                        "job_count": 2,
                        "command": 'echo "task_b" >&2',
                    },
                },
                "stderr_table_path": "//tmp/stderr"
            })

        table_stderrs = read_table("//tmp/stderr")
        table_stderrs_per_task = Counter(row["data"] for row in table_stderrs)

        job_ids = ls(op.get_path() + "/jobs")
        cypress_stderrs_per_task = Counter(read_file(op.get_path() + "/jobs/{0}/stderr".format(job_id)) for job_id in job_ids)

        assert dict(table_stderrs_per_task) == {"task_a\n": 3, "task_b\n": 2}
        assert dict(cypress_stderrs_per_task) == {"task_a\n": 3, "task_b\n": 2}

    @authors("max42")
    def test_fail_on_failed_job(self):
        with pytest.raises(YtError):
            op = vanilla(
                spec={
                    "tasks": {
                        "task_a": {
                            "job_count": 2,
                            "command": 'if [[ "$YT_JOB_INDEX" == 2 ]] ; then exit 1; fi',
                        },
                        "task_b": {
                            "job_count": 1,
                            "command": 'if [[ "$YT_JOB_INDEX" == 2 ]] ; then exit 1; fi',
                        },
                    },
                    "fail_on_job_restart": True,
                })

    @authors("max42")
    def test_revival_with_fail_on_job_restart(self):
        op = vanilla(
            dont_track=True,
            spec={
                "tasks": {
                    "task_a": {
                        "job_count": 1,
                        "command": with_breakpoint("BREAKPOINT"),
                    },
                    "task_b": {
                        "job_count": 1,
                        "command": with_breakpoint("BREAKPOINT"),
                    },
                },
                "fail_on_job_restart": True,
            })
        wait(lambda: len(op.get_running_jobs()) == 2)
        time.sleep(1.0)
        # By this moment all 2 running jobs made it to snapshot, so operation will not fail on revival.
        with Restarter(self.Env, SCHEDULERS_SERVICE):
            pass
        release_breakpoint()
        op.track()

        with pytest.raises(YtError):
            op = vanilla(
                dont_track=True,
                spec={
                    "tasks": {
                        "task_a": {
                            "job_count": 1,
                            "command": "", # do nothing
                        },
                        "task_b": {
                            "job_count": 6,
                            "command": with_breakpoint("BREAKPOINT"),
                        },
                    },
                    "fail_on_job_restart": True,
                })
            # 6 jobs may not be running simultaneously, so the snapshot will contain information about
            # at most 5 running jobs plus 1 completed job, leading to operation fail on revival.
            time.sleep(1.0)
            with Restarter(self.Env, SCHEDULERS_SERVICE):
                pass
            op.track()

    @authors("max42")
    def test_abandon_job(self):
        # Abandoning vanilla job is ok.
        op = vanilla(
            dont_track=True,
            spec={
                "tasks": {
                    "tasks_a": {
                        "job_count": 1,
                        "command": with_breakpoint("BREAKPOINT ; exit 0"),
                    }
                },
                "fail_on_job_restart": True
            })
        job_id = wait_breakpoint()[0]
        jobs = op.get_running_jobs()
        assert len(jobs) == 1
        abandon_job(job_id)
        release_breakpoint()
        op.track()

    @authors("max42")
    def test_non_interruptible(self):
        op = vanilla(
            dont_track=True,
            spec={
                "tasks": {
                    "tasks_a": {
                        "job_count": 1,
                        "command": with_breakpoint("BREAKPOINT ; exit 0"),
                    }
                },
                "fail_on_job_restart": True
            })
        wait_breakpoint()
        jobs = list(op.get_running_jobs())
        assert len(jobs) == 1
        job_id = jobs[0]
        with pytest.raises(YtError):
            interrupt_job(job_id)

    # TODO(max42): add lambda job: signal_job(job, "SIGKILL") when YT-8243 is fixed.
    @authors("max42")
    @pytest.mark.parametrize("action", [abort_job])
    def test_fail_on_manually_stopped_job(self, action):
        with pytest.raises(YtError):
            op = vanilla(
                dont_track=True,
                spec={
                    "tasks": {
                        "task_a": {
                            "job_count": 1,
                            "command": " ; ".join([events_on_fs().notify_event_cmd("job_started_a"), events_on_fs().wait_event_cmd("finish_a")]),
                        },
                        "task_b": {
                            "job_count": 1,
                            "command": " ; ".join([events_on_fs().notify_event_cmd("job_started_b"), events_on_fs().wait_event_cmd("finish_b")]),
                        },
                    },
                    "fail_on_job_restart": True,
                })
            events_on_fs().wait_event("job_started_a")
            events_on_fs().wait_event("job_started_b")
            jobs = list(op.get_running_jobs())
            assert len(jobs) == 2
            job_id = jobs[0]
            action(job_id)
            events_on_fs().notify_event("finish_a")
            events_on_fs().notify_event("finish_b")
            op.track()

    @authors("max42")
    def test_table_output(self):
        create("table", "//tmp/t_ab") # append = %true
        create("table", "//tmp/t_bc") # sorted_by = [a]
        create("table", "//tmp/t_ac") # regular
        write_table("//tmp/t_ab", [{"a": 1}])
        vanilla(
            spec={
                "tasks": {
                    "task_a": {
                        "job_count": 1,
                        "output_table_paths": ["<append=%true>//tmp/t_ab", "//tmp/t_ac"],
                        "command": "echo '{a=20}' >&1; echo '{a=9}' >&4",
                    },
                    "task_b": {
                        "job_count": 1,
                        "output_table_paths": ["<sorted_by=[a]>//tmp/t_bc", "<append=%true>//tmp/t_ab"],
                        "command": "echo '{a=7}' >&1; echo '{a=5}' >&4",
                    },
                    "task_c": {
                        "job_count": 1,
                        "output_table_paths": ["//tmp/t_ac", "<sorted_by=[a]>//tmp/t_bc"],
                        "command": "echo '{a=3}' >&1; echo '{a=6}' >&4",
                    }
                }
            })
        assert read_table("//tmp/t_ab") in [[{"a": 1}, {"a": 20}, {"a": 5}],
                                            [{"a": 1}, {"a": 5}, {"a": 20}]]
        assert read_table("//tmp/t_bc") == [{"a": 6}, {"a": 7}]
        assert read_table("//tmp/t_ac") in [[{"a": 3}, {"a": 9}],
                                            [{"a": 9}, {"a": 3}]]

    @authors("max42")
    def test_format(self):
        create("table", "//tmp/t")
        vanilla(
            spec={
                "tasks": {
                    "task_a": {
                        "job_count": 1,
                        "output_table_paths": ["//tmp/t"],
                        "format": "yson",
                        "command": "echo '{a=1}'",
                    },
                    "task_b": {
                        "job_count": 1,
                        "output_table_paths": ["//tmp/t"],
                        "format": "json",
                        "command": "echo \"{\\\"a\\\": 2}\"",
                    },
                }
            })
        assert sorted(read_table("//tmp/t")) == [{"a": 1}, {"a": 2}]

    @authors("max42")
    def test_attribute_validation_for_duplicated_output_tables(self):
        create("table", "//tmp/t")
        with pytest.raises(YtError):
            op = vanilla(
                spec={
                    "tasks": {
                        "task_a": {
                            "job_count": 1,
                            "command": "true",
                            "output_table_paths": ["<append=%true>//tmp/t"],
                        },
                        "task_b": {
                            "job_count": 1,
                            "command": "true",
                            "output_table_paths": ["<append=%false>//tmp/t"],
                        },
                    },
                    "fail_on_job_restart": True,
                })

    @authors("max42")
    def test_operation_limits(self):
        with pytest.raises(YtError):
            vanilla(spec={"tasks": {"task_" + str(i): {"job_count": 1, "command": "true"} for i in range(101)}})
        with pytest.raises(YtError):
            vanilla(spec={"tasks": {"main": {"job_count": 100 * 1000 + 1, "command": "true"}}})

##################################################################

class TestSchedulerVanillaCommandsMulticell(TestSchedulerVanillaCommands):
    UM_SECONDARY_MASTER_CELLS = 2
