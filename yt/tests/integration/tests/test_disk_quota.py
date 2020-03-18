import yt_env_setup
from yt_env_setup import wait, get_porto_delta_node_config, patch_porto_env_only, YTEnvSetup
from yt_commands import *

from quota_mixin import QuotaMixin

import yt.common

import pytest

import os
import shutil

##################################################################

class TestDiskQuota(QuotaMixin):
    NUM_SCHEDULERS = 1
    NUM_MASTERS = 1
    NUM_NODES = 3

    REQUIRE_YTSERVER_ROOT_PRIVILEGES = True

    def _init_tables(self):
        tables = ["//tmp/t1", "//tmp/t2"]
        for table in tables:
            create("table", table)
        write_table(tables[0], [{"foo": "bar"} for _ in xrange(200)])
        return tables

    @authors("astiunov")
    def test_disk_usage(self):
        tables = self._init_tables()
        try:
            map(
                in_=tables[0],
                out=tables[1],
                command="/bin/bash -c 'dd if=/dev/zero of=zeros.txt count=20'",
                spec={"mapper": {"disk_space_limit": 2 * 1024}, "max_failed_job_count": 1}
            )
        except YtError as err:
            message = str(err)
            if "quota exceeded" not in message:
                raise
        else:
            assert False, "Operation expected to fail, but completed successfully"

    @authors("astiunov")
    def test_inodes_count(self):
        tables = self._init_tables()
        try:
            map(
                in_=tables[0],
                out=tables[1],
                command="/bin/bash -c 'touch {1..200}.txt'",
                spec={"mapper": {"inode_limit": 100}, "max_failed_job_count": 1}
            )
        except YtError as err:
            message = str(err)
            if "quota exceeded" not in message:
                raise
        else:
            assert False, "Operation expected to fail, but completed successfully"

##################################################################

class BaseTestDiskUsage(object):
    NUM_SCHEDULERS = 1
    NUM_MASTERS = 1
    NUM_NODES = 1
    DELTA_NODE_CONFIG_BASE = {
        "exec_agent": {
            "slot_manager": {
                "locations": [
                    {
                        "disk_quota": 1024 * 1024,
                        "disk_usage_watermark": 0
                    }
                ],
                "disk_resources_update_period": 100,
            },
            "job_controller": {
                "waiting_jobs_timeout": 1000,
                "resource_limits": {
                    "user_slots": 3,
                    "cpu": 3.0
                }
            },
            "min_required_disk_space": 0,
        }
    }

    DELTA_MASTER_CONFIG = {
        "cypress_manager": {
            "default_table_replication_factor": 1
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "safe_scheduler_online_time": 500,
        }
    }

    REQUIRE_YTSERVER_ROOT_PRIVILEGES = True

    def _init_tables(self):
        tables = ["//tmp/t1", "//tmp/t2", "//tmp/t3"]
        for table in tables:
            create("table", table)
        write_table(tables[0], [{"foo": "bar"} for _ in xrange(10)])
        return tables

    def run_test(self, tables, fatty_options):
        options = {
            "in_": tables[0],
            "out": tables[1],
            "track": False,
        }

        options.update(fatty_options)

        first = map(**options)

        events_on_fs().wait_event("file_written")

        check_op = {
            "in_": tables[0],
            "out": tables[2],
            "command": "true",
            "spec": {"mapper": {"disk_space_limit": 1024 * 1024 / 2}, "max_failed_job_count": 1}
        }

        op = map(track=False, **check_op)
        wait(lambda: exists(op.get_path() + "/controller_orchid/progress/jobs"))
        for type in ("running", "aborted", "failed"):
            assert op.get_job_count(type) == 0
        op.abort()

        events_on_fs().notify_event("finish_job")
        first.track()

        map(**check_op)

    @authors("astiunov")
    def test_lack_space_node(self):
        tables = self._init_tables()
        options = {
            "command": " ; ".join([
                "dd if=/dev/zero of=zeros.txt count=1500",
                events_on_fs().notify_event_cmd("file_written"),
                events_on_fs().wait_event_cmd("finish_job"),
            ])
        }

        self.run_test(tables, options)

    @authors("astiunov")
    def test_lack_space_node_with_quota(self):
        tables = self._init_tables()
        options = {
            "command": " ; ".join([
                "true",
                events_on_fs().notify_event_cmd("file_written"),
                events_on_fs().wait_event_cmd("finish_job"),
            ]),
            "spec": {"mapper": {"disk_space_limit": 1024 * 1024 * 2 / 3}, "max_failed_job_count": 1}
        }

        self.run_test(tables, options)

    @authors("ignat")
    def test_not_available_nodes(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"foo": "bar"}])

        op = map(track=False, command="cat", in_="//tmp/t1", out="//tmp/t2",
                 spec={"mapper": {"disk_space_limit": 2 * 1024 * 1024}, "max_failed_job_count": 1})
        op.ensure_running()

        time.sleep(1.0)

        # NB: We have no sanity checks for disk space in scheduler
        for type in ("running", "aborted", "failed"):
            assert op.get_job_count(type) == 0

    @authors("ignat")
    def test_scheduled_after_wait(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")
        write_table("//tmp/t1", [{"foo": "bar"}])

        op1 = map(track=False, command="sleep 1000", in_="//tmp/t1", out="//tmp/t2",
                  spec={"mapper": {"disk_space_limit": 2 * 1024 * 1024 / 3}, "max_failed_job_count": 1})
        op1.ensure_running()
        wait(lambda: op1.get_job_count("running") == 1)


        op2 = map(track=False, command="sleep 1000", in_="//tmp/t1", out="//tmp/t3",
                  spec={"mapper": {"disk_space_limit": 2 * 1024 * 1024 / 3}, "max_failed_job_count": 1})
        op2.ensure_running()
        for type in ("running", "aborted", "failed"):
            assert op2.get_job_count(type) == 0

        op1.abort()

        wait(lambda: op2.get_job_count("running") == 1)
        op2.abort()

@patch_porto_env_only(BaseTestDiskUsage)
class TestDiskUsageQuota(BaseTestDiskUsage, QuotaMixin):
    DELTA_NODE_CONFIG = BaseTestDiskUsage.DELTA_NODE_CONFIG_BASE

@patch_porto_env_only(BaseTestDiskUsage)
class TestDiskUsagePorto(BaseTestDiskUsage, YTEnvSetup):
    DELTA_NODE_CONFIG = yt.common.update(
        get_porto_delta_node_config(),
        BaseTestDiskUsage.DELTA_NODE_CONFIG_BASE
    )
    REQUIRE_YTSERVER_ROOT_PRIVILEGES = True
    USE_PORTO_FOR_SERVERS = True

    @classmethod
    def modify_node_config(cls, config):
        if yt_env_setup.SANDBOX_STORAGE_ROOTDIR is None:
            pytest.skip("SANDBOX_STORAGE_ROOTDIR should be specified for tests with disk quotas in porto")

        cls.run_name = os.path.basename(cls.path_to_run)
        cls.disk_path = os.path.join(yt_env_setup.SANDBOX_STORAGE_ROOTDIR, cls.run_name, "disk_default")
        os.makedirs(cls.disk_path)

        config["exec_agent"]["slot_manager"]["locations"][0]["path"] = cls.disk_path

    @classmethod
    def teardown_class(cls):
        super(TestDiskUsagePorto, cls).teardown_class()
        if yt_env_setup.SANDBOX_STORAGE_ROOTDIR is not None:
            shutil.rmtree(os.path.join(yt_env_setup.SANDBOX_STORAGE_ROOTDIR, cls.run_name))

##################################################################
