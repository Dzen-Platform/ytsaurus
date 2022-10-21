from yt_env_setup import (YTEnvSetup, Restarter, QUEUE_AGENTS_SERVICE)

from yt_commands import (authors, get, set, ls, wait, assert_yt_error, create, sync_mount_table, sync_create_cells,
                         insert_rows, delete_rows, remove, raises_yt_error, exists, start_transaction, select_rows,
                         sync_unmount_table, sync_reshard_table, trim_rows, print_debug, abort_transaction)

from yt.common import YtError, update_inplace, update

import copy
import datetime
import time
import pytz

import pytest

from yt.yson import YsonUint64, YsonEntity

import yt_error_codes

from yt.wrapper.ypath import escape_ypath_literal

import yt.environment.init_queue_agent_state as init_queue_agent_state

##################################################################


BIGRT_CONSUMER_TABLE_SCHEMA = [
    {"name": "ShardId", "type": "uint64", "sort_order": "ascending"},
    {"name": "Offset", "type": "uint64"},
]


class OrchidBase:
    def __init__(self, agent_id=None):
        if agent_id is None:
            agent_ids = ls("//sys/queue_agents/instances", verbose=False)
            assert len(agent_ids) == 1
            agent_id = agent_ids[0]

        self.agent_id = agent_id

    def queue_agent_orchid_path(self):
        return "//sys/queue_agents/instances/" + self.agent_id + "/orchid"


class QueueAgentOrchid(OrchidBase):
    @staticmethod
    def leader_orchid():
        agent_ids = ls("//sys/queue_agents/instances", verbose=False)
        for agent_id in agent_ids:
            if get("//sys/queue_agents/instances/{}/orchid/queue_agent/active".format(agent_id)):
                return QueueAgentOrchid(agent_id)
        assert False, "No leading queue agent found"

    def get_active(self):
        return get(self.queue_agent_orchid_path() + "/queue_agent/active")

    def get_poll_index(self):
        return get(self.queue_agent_orchid_path() + "/queue_agent/poll_index")

    def get_poll_error(self):
        return YtError.from_dict(get(self.queue_agent_orchid_path() + "/queue_agent/poll_error"))

    def wait_fresh_poll(self):
        poll_index = self.get_poll_index()
        wait(lambda: self.get_poll_index() >= poll_index + 2)

    def validate_no_poll_error(self):
        error = self.get_poll_error()
        if error.code != 0:
            raise error

    def get_queues(self):
        self.wait_fresh_poll()
        return get(self.queue_agent_orchid_path() + "/queue_agent/queues")

    def get_consumers(self):
        self.wait_fresh_poll()
        return get(self.queue_agent_orchid_path() + "/queue_agent/consumers")

    def get_queue_pass_index(self, queue_ref):
        return get(self.queue_agent_orchid_path() + "/queue_agent/queues/" +
                   escape_ypath_literal(queue_ref) + "/pass_index")

    def get_consumer_pass_index(self, consumer_ref):
        return get(self.queue_agent_orchid_path() + "/queue_agent/consumers/" +
                   escape_ypath_literal(consumer_ref) + "/pass_index")

    def wait_fresh_queue_pass(self, queue_ref):
        pass_index = self.get_queue_pass_index(queue_ref)
        wait(lambda: self.get_queue_pass_index(queue_ref) >= pass_index + 2)

    def wait_fresh_consumer_pass(self, consumer_ref):
        pass_index = self.get_consumer_pass_index(consumer_ref)
        wait(lambda: self.get_consumer_pass_index(consumer_ref) >= pass_index + 2)

    def get_queue_status(self, queue_ref):
        return get(self.queue_agent_orchid_path() + "/queue_agent/queues/" +
                   escape_ypath_literal(queue_ref) + "/status")

    def get_queue_partitions(self, queue_ref):
        return get(self.queue_agent_orchid_path() + "/queue_agent/queues/" +
                   escape_ypath_literal(queue_ref) + "/partitions")

    def get_queue(self, queue_ref):
        result = get(self.queue_agent_orchid_path() + "/queue_agent/queues/" +
                     escape_ypath_literal(queue_ref))
        return result["status"], result["partitions"]

    def get_consumer_status(self, consumer_ref):
        return get(self.queue_agent_orchid_path() + "/queue_agent/consumers/" +
                   escape_ypath_literal(consumer_ref) + "/status")

    def get_consumer_partitions(self, consumer_ref):
        return get(self.queue_agent_orchid_path() + "/queue_agent/consumers/" +
                   escape_ypath_literal(consumer_ref) + "/partitions")

    def get_consumer(self, consumer_ref):
        result = get(self.queue_agent_orchid_path() + "/queue_agent/consumers/" +
                     escape_ypath_literal(consumer_ref))
        return result["status"], result["partitions"]


class AlertManagerOrchid(OrchidBase):
    def __init__(self, agent_id=None):
        super(AlertManagerOrchid, self).__init__(agent_id)

    def get_alerts(self):
        return get("{}/alerts".format(self.queue_agent_orchid_path()))


class TestQueueAgentBase(YTEnvSetup):
    USE_DYNAMIC_TABLES = True

    @classmethod
    def modify_queue_agent_config(cls, config):
        update_inplace(config, {
            "cluster_connection": {
                # Disable cache.
                "table_mount_cache": {
                    "expire_after_successful_update_time": 0,
                    "expire_after_failed_update_time": 0,
                    "expire_after_access_time": 0,
                    "refresh_time": 0,
                },
                "transaction_manager": {
                    "retry_attempts": 1,
                },
            },
        })

    @classmethod
    def setup_class(cls):
        super(TestQueueAgentBase, cls).setup_class()

        queue_agent_config = cls.Env._cluster_configuration["queue_agent"][0]
        cls.root_path = queue_agent_config.get("root", "//sys/queue_agents")
        cls.config_path = queue_agent_config.get("dynamic_config_path", cls.root_path + "/config")

        if not exists(cls.config_path):
            create("document", cls.config_path, attributes={"value": {}})

        cls._apply_dynamic_config_patch(getattr(cls, "DELTA_QUEUE_AGENT_DYNAMIC_CONFIG", dict()))

    def setup_method(self, method):
        super(TestQueueAgentBase, self).setup_method(method)

        self._prepare_tables()

    def teardown_method(self, method):
        self._drop_tables()

        super(TestQueueAgentBase, self).teardown_method(method)

    @classmethod
    def _apply_dynamic_config_patch(cls, patch):
        config = get(cls.config_path)
        update_inplace(config, patch)
        set(cls.config_path, config)

        instances = ls(cls.root_path + "/instances")

        def config_updated_on_all_instances():
            for instance in instances:
                effective_config = get(
                    "{}/instances/{}/orchid/dynamic_config_manager/effective_config".format(cls.root_path, instance))
                if update(effective_config, config) != effective_config:
                    return False
            return True

        wait(config_updated_on_all_instances)

    def _prepare_tables(self, queue_table_schema=None, consumer_table_schema=None):
        sync_create_cells(1)
        remove("//sys/queue_agents/queues", force=True)
        remove("//sys/queue_agents/consumers", force=True)
        init_queue_agent_state.create_tables(
            self.Env.create_native_client(),
            queue_table_schema=queue_table_schema,
            consumer_table_schema=consumer_table_schema)
        sync_mount_table("//sys/queue_agents/queues")
        sync_mount_table("//sys/queue_agents/consumers")

    def _drop_tables(self):
        remove("//sys/queue_agents/queues", force=True)
        remove("//sys/queue_agents/consumers", force=True)

    def _create_queue(self, path, partition_count=1, enable_timestamp_column=True,
                      enable_cumulative_data_weight_column=True, **kwargs):
        schema = [{"name": "data", "type": "string"}]
        if enable_timestamp_column:
            schema += [{"name": "$timestamp", "type": "uint64"}]
        if enable_cumulative_data_weight_column:
            schema += [{"name": "$cumulative_data_weight", "type": "int64"}]
        attributes = {
            "dynamic": True,
            "schema": schema,
        }
        attributes.update(kwargs)
        create("table", path, attributes=attributes)
        if partition_count != 1:
            sync_reshard_table(path, partition_count)
        sync_mount_table(path)

    def _create_bigrt_consumer(self, path, target_queue, **kwargs):
        attributes = {
            "dynamic": True,
            "schema": BIGRT_CONSUMER_TABLE_SCHEMA,
            "target_queue": target_queue,
            "treat_as_queue_consumer": True,
        }
        attributes.update(kwargs)
        create("table", path, attributes=attributes)
        sync_mount_table(path)

    def _advance_bigrt_consumer(self, path, tablet_index, next_row_index):
        # Keep in mind that in BigRT offset points to the last read row rather than first unread row.
        # Initial status is represented as a missing row.
        if next_row_index == 0:
            delete_rows(path, [{"ShardId": tablet_index}])
        else:
            insert_rows(path, [{"ShardId": tablet_index, "Offset": next_row_index - 1}])

    @staticmethod
    def _wait_for_row_count(path, tablet_index, count):
        wait(lambda: len(select_rows(f"* from [{path}] where [$tablet_index] = {tablet_index}")) == count)


class TestQueueAgent(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
            "controller": {
                "pass_period": 100,
            },
        },
        "cypress_synchronizer": {
            "poll_period": 100,
            "policy": "watching",
            "clusters": ["primary"],
        },
    }

    @authors("achulkov2")
    def test_other_stages_are_ignored(self):
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()
        queue_orchid = QueueAgentOrchid()

        self._create_queue("//tmp/q")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_orchid.wait_fresh_poll()

        status = queue_orchid.get_queue_status("primary://tmp/q")
        assert status["partition_count"] == 1

        set("//tmp/q/@queue_agent_stage", "testing")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_orchid.wait_fresh_poll()

        with raises_yt_error(code=yt_error_codes.ResolveErrorCode):
            queue_orchid.get_queue_status("primary://tmp/q")

        set("//tmp/q/@queue_agent_stage", "production")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_orchid.wait_fresh_poll()

        status = queue_orchid.get_queue_status("primary://tmp/q")
        assert status["partition_count"] == 1


class TestQueueAgentNoSynchronizer(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
        },
        "cypress_synchronizer": {
            "enable": False,
        },
    }

    @authors("max42")
    def test_polling_loop(self):
        orchid = QueueAgentOrchid()

        self._drop_tables()

        orchid.wait_fresh_poll()
        assert_yt_error(orchid.get_poll_error(), yt_error_codes.ResolveErrorCode)

        wrong_schema = copy.deepcopy(init_queue_agent_state.QUEUE_TABLE_SCHEMA)
        for i in range(len(wrong_schema)):
            if wrong_schema[i]["name"] == "object_type":
                wrong_schema[i]["type"] = "int64"
                break
        self._prepare_tables(queue_table_schema=wrong_schema)

        orchid.wait_fresh_poll()
        assert_yt_error(orchid.get_poll_error(), "Row range schema is incompatible with queue table row schema")

        self._prepare_tables()
        orchid.wait_fresh_poll()
        orchid.validate_no_poll_error()

    @authors("max42")
    def test_queue_state(self):
        orchid = QueueAgentOrchid()

        orchid.wait_fresh_poll()
        queues = orchid.get_queues()
        assert len(queues) == 0

        # Missing row revision.
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "queue_agent_stage": "production"}])
        orchid.wait_fresh_poll()
        status = orchid.get_queue_status("primary://tmp/q")
        assert_yt_error(YtError.from_dict(status["error"]), "Queue is not in-sync yet")
        assert "type" not in status

        # Missing object type.
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(1234)}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_queue_status("primary://tmp/q")
        assert_yt_error(YtError.from_dict(status["error"]), "Queue is not in-sync yet")
        assert "type" not in status

        # Wrong object type.
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(2345), "object_type": "map_node"}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_queue_status("primary://tmp/q")
        assert_yt_error(YtError.from_dict(status["error"]), 'Invalid queue object type "map_node"')

        # Sorted dynamic table.
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(3456), "object_type": "table", "dynamic": True, "sorted": True}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_queue_status("primary://tmp/q")
        assert_yt_error(YtError.from_dict(status["error"]), "Only ordered dynamic tables are supported as queues")

        # Proper ordered dynamic table.
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(4567), "object_type": "table", "dynamic": True, "sorted": False}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_queue_status("primary://tmp/q")
        # This error means that controller is instantiated and works properly (note that //tmp/q does not exist yet).
        assert_yt_error(YtError.from_dict(status["error"]), code=yt_error_codes.ResolveErrorCode)

        # Switch back to sorted dynamic table.
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(5678), "object_type": "table", "dynamic": False, "sorted": False}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_queue_status("primary://tmp/q")
        assert_yt_error(YtError.from_dict(status["error"]), "Only ordered dynamic tables are supported as queues")
        assert "family" not in status

        # Remove row; queue should be unregistered.
        delete_rows("//sys/queue_agents/queues", [{"cluster": "primary", "path": "//tmp/q"}])
        orchid.wait_fresh_poll()

        queues = orchid.get_queues()
        assert len(queues) == 0

    @authors("max42")
    def test_consumer_state(self):
        orchid = QueueAgentOrchid()

        orchid.wait_fresh_poll()
        queues = orchid.get_queues()
        consumers = orchid.get_consumers()
        assert len(queues) == 0
        assert len(consumers) == 0

        # Missing row revision.
        insert_rows("//sys/queue_agents/consumers",
                    [{"cluster": "primary", "path": "//tmp/c", "queue_agent_stage": "production"}])
        orchid.wait_fresh_poll()
        status = orchid.get_consumer_status("primary://tmp/c")
        assert_yt_error(YtError.from_dict(status["error"]), "Consumer is not in-sync yet")
        assert "target" not in status

        # Missing target.
        insert_rows("//sys/queue_agents/consumers",
                    [{"cluster": "primary", "path": "//tmp/c", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(1234)}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_consumer_status("primary://tmp/c")
        assert_yt_error(YtError.from_dict(status["error"]), "Consumer is missing target")
        assert "target" not in status

        # Unregistered target queue.
        insert_rows("//sys/queue_agents/consumers",
                    [{"cluster": "primary", "path": "//tmp/c", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(2345), "target_cluster": "primary", "target_path": "//tmp/q",
                      "treat_as_queue_consumer": True}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_consumer_status("primary://tmp/c")
        assert_yt_error(YtError.from_dict(status["error"]), 'Target queue "primary://tmp/q" is not registered')

        # Register target queue with wrong object type.
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "queue_agent_stage": "production",
                      "row_revision": YsonUint64(4567), "object_type": "map_node", "dynamic": True, "sorted": False}],
                    update=True)
        orchid.wait_fresh_poll()
        status = orchid.get_queue_status("primary://tmp/q")
        assert_yt_error(YtError.from_dict(status["error"]), 'Invalid queue object type "map_node"')

        # Queue error is propagated as consumer error.
        status = orchid.get_consumer_status("primary://tmp/c")
        assert_yt_error(YtError.from_dict(status["error"]), 'Invalid queue object type "map_node"')

    @authors("achulkov2")
    def test_alerts(self):
        orchid = QueueAgentOrchid()
        alert_orchid = AlertManagerOrchid()

        self._drop_tables()

        orchid.wait_fresh_poll()

        wait(lambda: "queue_agent_pass_failed" in alert_orchid.get_alerts())
        assert_yt_error(YtError.from_dict(alert_orchid.get_alerts()["queue_agent_pass_failed"]),
                        "Error polling queue state")

    @authors("achulkov2")
    def test_no_alerts(self):
        alert_orchid = AlertManagerOrchid()

        wait(lambda: not alert_orchid.get_alerts())


class TestOrchidSelfRedirect(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    DELTA_QUEUE_AGENT_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
        },
        "cypress_synchronizer": {
            "poll_period": 100,
        },
        "election_manager": {
            "transaction_timeout": 5000,
            "transaction_ping_period": 100,
            "lock_acquisition_period": 300000,
            "leader_cache_update_period": 100,
        },
    }

    @authors("achulkov2")
    def test_orchid_self_redirect(self):
        orchid = QueueAgentOrchid()
        wait(lambda: orchid.get_active())

        locks = get("//sys/queue_agents/leader_lock/@locks")
        assert len(locks) == 1
        assert locks[0]["state"] == "acquired"
        tx_id = locks[0]["transaction_id"]

        # Give the election manager time to store the current leader into its cache. Just in case.
        time.sleep(1)

        abort_transaction(tx_id)

        wait(lambda: not orchid.get_active())

        with raises_yt_error(code=yt_error_codes.ResolveErrorCode):
            orchid.get_queue_status("primary://tmp/q")


class TestQueueController(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
            "controller": {
                "pass_period": 100,
            },
        },
        "cypress_synchronizer": {
            "poll_period": 100,
            "policy": "watching",
            "clusters": ["primary"],
        },
    }

    def _timestamp_to_iso_str(self, ts):
        unix_ts = ts >> 30
        dt = datetime.datetime.fromtimestamp(unix_ts, tz=pytz.UTC)
        return dt.isoformat().replace("+00:00", ".000000Z")

    @authors("max42")
    def test_queue_status(self):
        orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q", partition_count=2)
        self._create_bigrt_consumer("//tmp/c", "primary://tmp/q")

        cypress_synchronizer_orchid.wait_fresh_poll()
        orchid.wait_fresh_poll()
        orchid.wait_fresh_queue_pass("primary://tmp/q")

        queue_status = orchid.get_queue_status("primary://tmp/q")
        assert queue_status["family"] == "ordered_dynamic_table"
        assert queue_status["partition_count"] == 2
        assert queue_status["consumers"] == {"primary://tmp/c": {"vital": False, "owner": "root"}}

        def assert_partition(partition, lower_row_index, upper_row_index):
            assert partition["lower_row_index"] == lower_row_index
            assert partition["upper_row_index"] == upper_row_index
            assert partition["available_row_count"] == upper_row_index - lower_row_index

        null_time = "1970-01-01T00:00:00.000000Z"

        queue_partitions = orchid.get_queue_partitions("primary://tmp/q")
        for partition in queue_partitions:
            assert_partition(partition, 0, 0)
            assert partition["last_row_commit_time"] == null_time

        insert_rows("//tmp/q", [{"data": "foo", "$tablet_index": 0}])
        orchid.wait_fresh_queue_pass("primary://tmp/q")

        queue_partitions = orchid.get_queue_partitions("primary://tmp/q")
        assert_partition(queue_partitions[0], 0, 1)
        assert queue_partitions[0]["last_row_commit_time"] != null_time
        assert_partition(queue_partitions[1], 0, 0)

        trim_rows("//tmp/q", 0, 1)

        self._wait_for_row_count("//tmp/q", 0, 0)

        orchid.wait_fresh_queue_pass("primary://tmp/q")

        queue_partitions = orchid.get_queue_partitions("primary://tmp/q")
        assert_partition(queue_partitions[0], 1, 1)
        assert queue_partitions[0]["last_row_commit_time"] != null_time
        assert_partition(queue_partitions[1], 0, 0)

        insert_rows("//tmp/q", [{"data": "foo", "$tablet_index": 0}] * 100)

        orchid.wait_fresh_queue_pass("primary://tmp/q")

        queue_partitions = orchid.get_queue_partitions("primary://tmp/q")

        assert_partition(queue_partitions[0], 1, 101)
        assert queue_partitions[0]["cumulative_data_weight"] == 2020
        assert queue_partitions[0]["trimmed_data_weight"] <= 2 * 20

        trim_rows("//tmp/q", 0, 91)
        self._wait_for_row_count("//tmp/q", 0, 10)

        orchid.wait_fresh_queue_pass("primary://tmp/q")

        queue_partitions = orchid.get_queue_partitions("primary://tmp/q")

        assert_partition(queue_partitions[0], 91, 101)
        assert queue_partitions[0]["cumulative_data_weight"] == 2020
        assert 89 * 20 <= queue_partitions[0]["trimmed_data_weight"] <= 92 * 20
        assert 9 * 20 <= queue_partitions[0]["available_data_weight"] <= 11 * 20

    @authors("max42")
    def test_consumer_status(self):
        orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q", partition_count=2)
        self._create_bigrt_consumer("//tmp/c", "primary://tmp/q")

        cypress_synchronizer_orchid.wait_fresh_poll()
        orchid.wait_fresh_poll()

        insert_rows("//tmp/q", [{"data": "foo", "$tablet_index": 0}, {"data": "bar", "$tablet_index": 1}])
        time.sleep(1.5)
        insert_rows("//tmp/q", [{"data": "foo", "$tablet_index": 0}, {"data": "bar", "$tablet_index": 1}])
        timestamps = [row["ts"] for row in select_rows("[$timestamp] as ts from [//tmp/q]")]
        timestamps = sorted(timestamps)
        assert timestamps[0] == timestamps[1] and timestamps[2] == timestamps[3]
        timestamps = [timestamps[0], timestamps[2]]
        print_debug(self._timestamp_to_iso_str(timestamps[0]), self._timestamp_to_iso_str(timestamps[1]))

        orchid.wait_fresh_consumer_pass("primary://tmp/c")
        consumer_status = orchid.get_consumer_status("primary://tmp/c")
        assert consumer_status["partition_count"] == 2
        assert not consumer_status["vital"]
        assert consumer_status["target_queue"] == "primary://tmp/q"
        assert consumer_status["owner"] == "root"

        time.sleep(1.5)
        self._advance_bigrt_consumer("//tmp/c", 0, 0)
        time.sleep(1.5)
        self._advance_bigrt_consumer("//tmp/c", 1, 0)

        def assert_partition(partition, next_row_index):
            assert partition["next_row_index"] == next_row_index
            assert partition["unread_row_count"] == max(0, 2 - next_row_index)
            assert partition["next_row_commit_time"] == (self._timestamp_to_iso_str(timestamps[next_row_index])
                                                         if next_row_index < 2 else YsonEntity())
            assert (partition["processing_lag"] > 0) == (next_row_index < 2)

        orchid.wait_fresh_consumer_pass("primary://tmp/c")
        consumer_partitions = orchid.get_consumer_partitions("primary://tmp/c")
        assert_partition(consumer_partitions[0], 0)
        assert_partition(consumer_partitions[1], 0)

        self._advance_bigrt_consumer("//tmp/c", 0, 1)
        orchid.wait_fresh_consumer_pass("primary://tmp/c")
        consumer_partitions = orchid.get_consumer_partitions("primary://tmp/c")
        assert_partition(consumer_partitions[0], 1)

        self._advance_bigrt_consumer("//tmp/c", 1, 2)
        orchid.wait_fresh_consumer_pass("primary://tmp/c")
        consumer_partitions = orchid.get_consumer_partitions("primary://tmp/c")
        assert_partition(consumer_partitions[1], 2)

    @authors("max42")
    def test_consumer_partition_disposition(self):
        orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q")
        self._create_bigrt_consumer("//tmp/c", "primary://tmp/q")

        cypress_synchronizer_orchid.wait_fresh_poll()
        orchid.wait_fresh_poll()

        insert_rows("//tmp/q", [{"data": "foo"}] * 3)
        trim_rows("//tmp/q", 0, 1)
        orchid.wait_fresh_queue_pass("primary://tmp/q")

        expected_dispositions = ["expired", "pending_consumption", "pending_consumption", "up_to_date", "ahead"]
        for offset, expected_disposition in enumerate(expected_dispositions):
            self._advance_bigrt_consumer("//tmp/c", 0, offset)
            orchid.wait_fresh_consumer_pass("primary://tmp/c")
            partition = orchid.get_consumer_partitions("primary://tmp/c")[0]
            assert partition["disposition"] == expected_disposition
            assert partition["unread_row_count"] == 3 - offset

    @authors("max42")
    def test_inconsistent_partitions_in_consumer_table(self):
        orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q", partition_count=2)
        self._create_bigrt_consumer("//tmp/c", "primary://tmp/q")

        cypress_synchronizer_orchid.wait_fresh_poll()
        orchid.wait_fresh_poll()

        insert_rows("//tmp/q", [{"data": "foo"}] * 2)
        orchid.wait_fresh_queue_pass("primary://tmp/q")

        self._advance_bigrt_consumer("//tmp/c", 1, 1)
        self._advance_bigrt_consumer("//tmp/c", 2 ** 63 - 1, 1)
        self._advance_bigrt_consumer("//tmp/c", 2 ** 64 - 1, 1)

        orchid.wait_fresh_consumer_pass("primary://tmp/c")

        partitions = orchid.get_consumer_partitions("primary://tmp/c")
        assert len(partitions) == 2
        assert partitions[0]["next_row_index"] == 0

    @authors("max42")
    def DISABLED_test_rates(self):
        eps = 1e-2
        zero = {"current": 0.0, "1m": 0.0, "1h": 0.0, "1d": 0.0}

        orchid = QueueAgentOrchid()

        self._create_queue("//tmp/q", partition_count=2)
        self._create_bigrt_consumer("//tmp/c", "primary://tmp/q")

        # We advance consumer in both partitions by one beforehand in order to workaround
        # the corner-case when consumer stands on the first row in the available row window.
        # In such case we (currently) cannot reliably know cumultive data weight.

        insert_rows("//tmp/q", [{"data": "x", "$tablet_index": 0}, {"data": "x", "$tablet_index": 1}])
        self._advance_bigrt_consumer("//tmp/c", 0, 1)
        self._advance_bigrt_consumer("//tmp/c", 1, 1)

        # Expose queue and consumer.

        insert_rows("//sys/queue_agents/consumers", [{"cluster": "primary", "path": "//tmp/c"}])
        insert_rows("//sys/queue_agents/queues", [{"cluster": "primary", "path": "//tmp/q"}])

        # Test queue write rate. Initially rates are zero.

        orchid.wait_fresh_poll()
        orchid.wait_fresh_queue_pass("primary://tmp/q")
        status, partitions = orchid.get_queue("primary://tmp/q")
        assert len(partitions) == 2
        assert partitions[0]["write_row_count_rate"] == zero
        assert partitions[1]["write_row_count_rate"] == zero
        assert partitions[0]["write_data_weight_rate"] == zero
        assert partitions[1]["write_data_weight_rate"] == zero
        assert status["write_row_count_rate"] == zero
        assert status["write_data_weight_rate"] == zero

        # After inserting (resp.) 2 and 1 rows, write rates should be non-zero and proportional to (resp.) 2 and 1.

        insert_rows("//tmp/q", [{"data": "x", "$tablet_index": 0}] * 2 + [{"data": "x", "$tablet_index": 1}])

        orchid.wait_fresh_queue_pass("primary://tmp/q")
        status, partitions = orchid.get_queue("primary://tmp/q")
        assert len(partitions) == 2
        assert partitions[1]["write_row_count_rate"]["1m"] > 0
        assert partitions[1]["write_data_weight_rate"]["1m"] > 0
        assert abs(partitions[0]["write_row_count_rate"]["1m"] - 2 * partitions[1]["write_row_count_rate"]["1m"]) < eps
        assert abs(partitions[0]["write_data_weight_rate"]["1m"] -
                   2 * partitions[1]["write_data_weight_rate"]["1m"]) < eps

        # Check total write rate.

        status, partitions = orchid.get_queue("primary://tmp/q")
        assert abs(status["write_row_count_rate"]["1m"] - 3 * partitions[1]["write_row_count_rate"]["1m"]) < eps
        assert abs(status["write_data_weight_rate"]["1m"] - 3 * partitions[1]["write_data_weight_rate"]["1m"]) < eps

        # Test consumer read rate. Again, initially rates are zero.

        orchid.wait_fresh_consumer_pass("primary://tmp/c")
        status, partitions = orchid.get_consumer("primary://tmp/c")
        assert len(partitions) == 2
        assert partitions[0]["read_row_count_rate"] == zero
        assert partitions[1]["read_row_count_rate"] == zero
        assert partitions[0]["read_data_weight_rate"] == zero
        assert partitions[1]["read_data_weight_rate"] == zero
        assert status["read_row_count_rate"] == zero
        assert status["read_data_weight_rate"] == zero

        # Advance consumer by (resp.) 2 and 1. Again, same statements should hold for read rates.

        self._advance_bigrt_consumer("//tmp/c", 0, 3)
        self._advance_bigrt_consumer("//tmp/c", 1, 2)

        orchid.wait_fresh_consumer_pass("primary://tmp/c")
        status, partitions = orchid.get_consumer("primary://tmp/c")
        assert len(partitions) == 2
        assert partitions[1]["read_row_count_rate"]["1m"] > 0
        assert abs(partitions[0]["read_row_count_rate"]["1m"] - 2 * partitions[1]["read_row_count_rate"]["1m"]) < eps
        assert partitions[1]["read_data_weight_rate"]["1m"] > 0
        assert abs(partitions[0]["read_data_weight_rate"]["1m"] -
                   2 * partitions[1]["read_data_weight_rate"]["1m"]) < eps

        # Check total read rate.

        status, partitions = orchid.get_consumer("primary://tmp/c")
        assert abs(status["read_row_count_rate"]["1m"] - 3 * partitions[1]["read_row_count_rate"]["1m"]) < eps
        assert abs(status["read_data_weight_rate"]["1m"] - 3 * partitions[1]["read_data_weight_rate"]["1m"]) < eps


class TestAutomaticTrimming(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
            "controller": {
                "pass_period": 100,
                "enable_automatic_trimming": True,
            },
        },
        "cypress_synchronizer": {
            "poll_period": 100,
            "policy": "watching",
            "clusters": ["primary"],
        },
    }

    @authors("achulkov2")
    def test_basic(self):
        queue_agent_orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q", partition_count=2)
        self._create_bigrt_consumer("//tmp/c1", "primary://tmp/q")
        self._create_bigrt_consumer("//tmp/c2", "primary://tmp/q")
        self._create_bigrt_consumer("//tmp/c3", "primary://tmp/q")

        set("//tmp/c1/@vital_queue_consumer", True)
        # c2 is not vital.
        set("//tmp/c3/@vital_queue_consumer", True)

        set("//tmp/q/@auto_trim_policy", "vital_consumers")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        select_rows("* from [//sys/queue_agents/queues]")

        insert_rows("//tmp/q", [{"$tablet_index": 0, "data": "foo"}] * 5)
        insert_rows("//tmp/q", [{"$tablet_index": 1, "data": "bar"}] * 7)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        self._advance_bigrt_consumer("//tmp/c1", 0, 3)
        self._advance_bigrt_consumer("//tmp/c2", 0, 5)
        # Vital consumer c3 was not advanced, so nothing should be trimmed.

        self._advance_bigrt_consumer("//tmp/c1", 1, 3)
        self._advance_bigrt_consumer("//tmp/c2", 1, 1)
        self._advance_bigrt_consumer("//tmp/c3", 1, 2)
        # Consumer c2 is non-vital, only c1 and c3 should be considered.

        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        self._wait_for_row_count("//tmp/q", 1, 5)
        self._wait_for_row_count("//tmp/q", 0, 5)

        # Consumer c3 is the farthest behind.
        self._advance_bigrt_consumer("//tmp/c3", 0, 2)

        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")
        self._wait_for_row_count("//tmp/q", 0, 3)

        # Now c1 is the farthest behind.
        self._advance_bigrt_consumer("//tmp/c3", 0, 4)

        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")
        self._wait_for_row_count("//tmp/q", 0, 2)

        # Both vital consumers are at the same offset.
        self._advance_bigrt_consumer("//tmp/c1", 1, 6)
        self._advance_bigrt_consumer("//tmp/c3", 1, 6)

        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")
        self._wait_for_row_count("//tmp/q", 1, 1)
        # Nothing should have changed here.
        self._wait_for_row_count("//tmp/q", 0, 2)

    @authors("achulkov2")
    def test_vitality_changes(self):
        queue_agent_orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q")
        self._create_bigrt_consumer("//tmp/c1", "primary://tmp/q")
        self._create_bigrt_consumer("//tmp/c2", "primary://tmp/q")
        self._create_bigrt_consumer("//tmp/c3", "primary://tmp/q")

        set("//tmp/c1/@vital_queue_consumer", True)
        # c2 is not vital.
        set("//tmp/c3/@vital_queue_consumer", True)

        set("//tmp/q/@auto_trim_policy", "vital_consumers")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        insert_rows("//tmp/q", [{"$tablet_index": 0, "data": "bar"}] * 7)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        # Only c1 and c3 are vital, so we should trim up to row 1.
        self._advance_bigrt_consumer("//tmp/c1", 0, 1)
        self._advance_bigrt_consumer("//tmp/c2", 0, 3)
        self._advance_bigrt_consumer("//tmp/c3", 0, 2)

        self._wait_for_row_count("//tmp/q", 0, 6)

        # Now we should only consider c3 and trim more rows.
        set("//tmp/c1/@vital_queue_consumer", False)
        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        self._wait_for_row_count("//tmp/q", 0, 5)

        # Now c2 and c3 are vital. Nothing should be trimmed though.
        set("//tmp/c2/@vital_queue_consumer", True)
        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        time.sleep(1)
        self._wait_for_row_count("//tmp/q", 0, 5)

        # Now only c2 is vital, so we should trim more rows.
        set("//tmp/c3/@vital_queue_consumer", False)
        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        self._wait_for_row_count("//tmp/q", 0, 4)

    @authors("achulkov2")
    def test_erroneous_vital_consumer(self):
        queue_agent_orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q")
        self._create_bigrt_consumer("//tmp/c1", "primary://tmp/q")
        self._create_bigrt_consumer("//tmp/c2", "primary://tmp/q")
        self._create_bigrt_consumer("//tmp/c3", "primary://tmp/q")

        set("//tmp/c1/@vital_queue_consumer", True)
        # c2 is not vital.
        set("//tmp/c3/@vital_queue_consumer", True)

        set("//tmp/q/@auto_trim_policy", "vital_consumers")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        insert_rows("//tmp/q", [{"$tablet_index": 0, "data": "foo"}] * 5)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        # Consumer c3 is vital, so nothing should be trimmed.
        self._advance_bigrt_consumer("//tmp/c1", 0, 2)
        self._advance_bigrt_consumer("//tmp/c2", 0, 1)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        time.sleep(1)
        self._wait_for_row_count("//tmp/q", 0, 5)

        # This should set an error in c1.
        sync_unmount_table("//tmp/c1")

        # Consumers c1 and c3 are vital, but c1 is in an erroneous state, so nothing should be trimmed.
        self._advance_bigrt_consumer("//tmp/c3", 0, 2)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        time.sleep(1)
        self._wait_for_row_count("//tmp/q", 0, 5)

        # Now c1 should be back and the first 2 rows should be trimmed.
        sync_mount_table("//tmp/c1")

        self._wait_for_row_count("//tmp/q", 0, 3)

    @authors("achulkov2")
    def test_erroneous_partition(self):
        queue_agent_orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q", partition_count=2)
        self._create_bigrt_consumer("//tmp/c1", "primary://tmp/q")
        self._create_bigrt_consumer("//tmp/c2", "primary://tmp/q")

        set("//tmp/c1/@vital_queue_consumer", True)
        # c2 is not vital.

        set("//tmp/q/@auto_trim_policy", "vital_consumers")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        insert_rows("//tmp/q", [{"$tablet_index": 0, "data": "foo"}] * 5)
        insert_rows("//tmp/q", [{"$tablet_index": 1, "data": "bar"}] * 7)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q")

        sync_unmount_table("//tmp/q", first_tablet_index=0, last_tablet_index=0)

        # Nothing should be trimmed from the first partition, since it is unmounted.
        self._advance_bigrt_consumer("//tmp/c1", 0, 2)
        self._advance_bigrt_consumer("//tmp/c2", 0, 1)

        # Yet the second partition should be trimmed based on c1.
        self._advance_bigrt_consumer("//tmp/c1", 1, 3)
        self._advance_bigrt_consumer("//tmp/c2", 1, 2)

        self._wait_for_row_count("//tmp/q", 1, 4)

        sync_mount_table("//tmp/q", first_tablet_index=0, last_tablet_index=0)
        # Now the first partition should be trimmed based on c1 as well.

        self._wait_for_row_count("//tmp/q", 0, 3)

    @authors("achulkov2")
    def test_erroneous_queue(self):
        queue_agent_orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q1")
        self._create_queue("//tmp/q2")
        self._create_bigrt_consumer("//tmp/c1", "primary://tmp/q1")
        self._create_bigrt_consumer("//tmp/c2", "primary://tmp/q2")

        set("//tmp/c1/@vital_queue_consumer", True)
        set("//tmp/c2/@vital_queue_consumer", True)

        set("//tmp/q1/@auto_trim_policy", "vital_consumers")
        set("//tmp/q2/@auto_trim_policy", "vital_consumers")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        insert_rows("//tmp/q1", [{"data": "foo"}] * 5)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q1")

        insert_rows("//tmp/q2", [{"data": "bar"}] * 7)
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q2")

        # Both queues should be trimmed since their sole consumers are vital.
        self._advance_bigrt_consumer("//tmp/c1", 0, 2)
        self._advance_bigrt_consumer("//tmp/c2", 0, 3)

        self._wait_for_row_count("//tmp/q1", 0, 3)
        self._wait_for_row_count("//tmp/q2", 0, 4)

        sync_unmount_table("//tmp/q2")

        self._advance_bigrt_consumer("//tmp/c1", 0, 3)
        self._advance_bigrt_consumer("//tmp/c2", 0, 4)

        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q1")
        queue_agent_orchid.wait_fresh_queue_pass("primary://tmp/q2")

        self._wait_for_row_count("//tmp/q1", 0, 2)
        # The second queue should not be trimmed yet.

        sync_mount_table("//tmp/q2")

        self._wait_for_row_count("//tmp/q2", 0, 3)

    @authors("achulkov2")
    def test_configuration_changes(self):
        queue_agent_orchid = QueueAgentOrchid()
        cypress_synchronizer_orchid = CypressSynchronizerOrchid()

        self._create_queue("//tmp/q")
        self._create_bigrt_consumer("//tmp/c", "primary://tmp/q")

        set("//tmp/c/@vital_queue_consumer", True)

        set("//tmp/q/@auto_trim_policy", "vital_consumers")

        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        insert_rows("//tmp/q", [{"$tablet_index": 0, "data": "bar"}] * 7)

        self._advance_bigrt_consumer("//tmp/c", 0, 1)
        self._wait_for_row_count("//tmp/q", 0, 6)

        set("//tmp/q/@auto_trim_policy", "none")
        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        self._advance_bigrt_consumer("//tmp/c", 0, 2)
        time.sleep(1)
        self._wait_for_row_count("//tmp/q", 0, 6)

        set("//tmp/q/@auto_trim_policy", "vital_consumers")
        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        self._wait_for_row_count("//tmp/q", 0, 5)

        remove("//tmp/q/@auto_trim_policy")
        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        self._advance_bigrt_consumer("//tmp/c", 0, 3)
        time.sleep(1)
        self._wait_for_row_count("//tmp/q", 0, 5)

        set("//tmp/q/@auto_trim_policy", "vital_consumers")
        cypress_synchronizer_orchid.wait_fresh_poll()
        queue_agent_orchid.wait_fresh_poll()

        self._wait_for_row_count("//tmp/q", 0, 4)

        self._apply_dynamic_config_patch({
            "queue_agent": {
                "controller": {
                    "enable_automatic_trimming": False,
                }
            }
        })

        self._advance_bigrt_consumer("//tmp/c", 0, 4)
        time.sleep(1)
        self._wait_for_row_count("//tmp/q", 0, 4)

        self._apply_dynamic_config_patch({
            "queue_agent": {
                "controller": {
                    "enable_automatic_trimming": True,
                }
            }
        })

        self._wait_for_row_count("//tmp/q", 0, 3)


class TestMultipleAgents(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 5

    USE_DYNAMIC_TABLES = True

    DELTA_QUEUE_AGENT_CONFIG = {
        "election_manager": {
            "transaction_timeout": 5000,
            "transaction_ping_period": 100,
            "lock_acquisition_period": 100,
            "leader_cache_update_period": 100,
        },
    }

    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
            "controller": {
                "pass_period": 100,
            },
        },
        "cypress_synchronizer": {
            "poll_period": 100,
        },
    }

    def _collect_leaders(self, instances, ignore_instances=()):
        result = []
        for instance in instances:
            if instance in ignore_instances:
                continue
            active = get("//sys/queue_agents/instances/" + instance + "/orchid/queue_agent/active")
            if active:
                result.append(instance)
        return result

    @authors("max42")
    def test_leader_election(self):
        instances = ls("//sys/queue_agents/instances")
        assert len(instances) == 5

        wait(lambda: len(self._collect_leaders(instances)) == 1)

        leader = self._collect_leaders(instances)[0]

        # Check that exactly one queue agent instance is performing polling.

        def validate_leader(leader, ignore_instances=()):
            wait(lambda: get("//sys/queue_agents/instances/" + leader + "/orchid/queue_agent/poll_index") > 10)

            for instance in instances:
                if instance != leader and instance not in ignore_instances:
                    assert get("//sys/queue_agents/instances/" + instance + "/orchid/queue_agent/poll_index") == 0

        validate_leader(leader)

        # Check that leader host is set in lock transaction attributes.

        locks = get("//sys/queue_agents/leader_lock/@locks")
        assert len(locks) == 5
        tx_id = None
        for lock in locks:
            if lock["state"] == "acquired":
                assert not tx_id
                tx_id = lock["transaction_id"]
        leader_from_tx_attrs = get("#" + tx_id + "/@host")

        assert leader == leader_from_tx_attrs

        # Test re-election.

        leader_index = get("//sys/queue_agents/instances/" + leader + "/@annotations/yt_env_index")

        with Restarter(self.Env, QUEUE_AGENTS_SERVICE, indexes=[leader_index]):
            prev_leader = leader

            def reelection():
                leaders = self._collect_leaders(instances, ignore_instances=(prev_leader,))
                return len(leaders) == 1 and leaders[0] != prev_leader
            wait(reelection)

            leader = self._collect_leaders(instances, ignore_instances=(prev_leader,))[0]

            validate_leader(leader, ignore_instances=(prev_leader,))

    @authors("achulkov2")
    def test_queue_attribute_leader_redirection(self):
        queues = ["//tmp/q{}".format(i) for i in range(3)]
        for queue in queues:
            create("table", queue, attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
            sync_mount_table(queue)
            insert_rows("//sys/queue_agents/queues", [{"cluster": "primary", "path": queue}])

        def wait_polls(cypress_synchronizer_orchid, queue_agent_orchid):
            cypress_synchronizer_orchid.wait_fresh_poll()
            queue_agent_orchid.wait_fresh_poll()
            for queue in queues:
                queue_agent_orchid.wait_fresh_queue_pass("primary:" + queue)

        wait_polls(CypressSynchronizerOrchid.leader_orchid(), QueueAgentOrchid.leader_orchid())

        instances = ls("//sys/queue_agents/instances")

        def perform_checks(ignore_instances=()):
            # Balancing channel should send request to random instances.
            for i in range(15):
                for queue in queues:
                    assert get(queue + "/@queue_status/partition_count") == 1

            for instance in instances:
                if instance in ignore_instances:
                    continue
                orchid_path = "//sys/queue_agents/instances/{}/orchid/queue_agent".format(instance)
                for queue in queues:
                    assert get("{}/queues/primary:{}/status/partition_count".format(
                        orchid_path, escape_ypath_literal(queue))) == 1

        perform_checks()

        leader = self._collect_leaders(instances)[0]
        leader_index = get("//sys/queue_agents/instances/" + leader + "/@annotations/yt_env_index")

        with Restarter(self.Env, QUEUE_AGENTS_SERVICE, indexes=[leader_index]):
            prev_leader = leader

            leaders = []

            def reelection():
                nonlocal leaders
                leaders = self._collect_leaders(instances, ignore_instances=(prev_leader,))
                return len(leaders) == 1 and leaders[0] != prev_leader

            wait(reelection)
            assert len(leaders) == 1

            wait_polls(CypressSynchronizerOrchid(leaders[0]), QueueAgentOrchid(leaders[0]))

            perform_checks(ignore_instances=(prev_leader,))

    @authors("achulkov2")
    def test_consumer_attribute_leader_redirection(self):
        consumers = ["//tmp/c{}".format(i) for i in range(3)]
        target_queues = ["//tmp/q{}".format(i) for i in range(len(consumers))]
        for i in range(len(consumers)):
            create("table", target_queues[i],
                   attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
            sync_mount_table(target_queues[i])
            create("table", consumers[i], attributes={"dynamic": True, "schema": BIGRT_CONSUMER_TABLE_SCHEMA,
                                                      "target_queue": "primary:" + target_queues[i],
                                                      "treat_as_queue_consumer": True})
            sync_mount_table(consumers[i])

            insert_rows("//sys/queue_agents/consumers", [{"cluster": "primary", "path": consumers[i]}])
            insert_rows("//sys/queue_agents/queues", [{"cluster": "primary", "path": target_queues[i]}])

        def wait_polls(cypress_synchronizer_orchid, queue_agent_orchid):
            cypress_synchronizer_orchid.wait_fresh_poll()
            queue_agent_orchid.wait_fresh_poll()
            for consumer in consumers:
                queue_agent_orchid.wait_fresh_consumer_pass("primary:" + consumer)

        wait_polls(CypressSynchronizerOrchid.leader_orchid(), QueueAgentOrchid.leader_orchid())

        instances = ls("//sys/queue_agents/instances")

        def perform_checks(ignore_instances=()):
            # Balancing channel should send request to random instances.
            for i in range(15):
                for consumer in consumers:
                    assert get(consumer + "/@queue_consumer_status/partition_count") == 1

            for instance in instances:
                if instance in ignore_instances:
                    continue
                orchid_path = "//sys/queue_agents/instances/{}/orchid/queue_agent".format(instance)
                for consumer in consumers:
                    assert get("{}/consumers/primary:{}/status/partition_count".format(
                        orchid_path, escape_ypath_literal(consumer))) == 1

        perform_checks()

        leader = self._collect_leaders(instances)[0]
        leader_index = get("//sys/queue_agents/instances/" + leader + "/@annotations/yt_env_index")

        with Restarter(self.Env, QUEUE_AGENTS_SERVICE, indexes=[leader_index]):
            prev_leader = leader

            leaders = []

            def reelection():
                nonlocal leaders
                leaders = self._collect_leaders(instances, ignore_instances=(prev_leader,))
                return len(leaders) == 1 and leaders[0] != prev_leader

            wait(reelection)
            assert len(leaders) == 1

            wait_polls(CypressSynchronizerOrchid(leaders[0]), QueueAgentOrchid(leaders[0]))

            perform_checks(ignore_instances=(prev_leader,))


class TestMasterIntegration(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    DELTA_QUEUE_AGENT_CONFIG = {
        "election_manager": {
            "transaction_timeout": 5000,
            "transaction_ping_period": 100,
            "lock_acquisition_period": 100,
        },
    }

    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
        },
        "cypress_synchronizer": {
            "poll_period": 100,
            "policy": "polling",
        },
    }

    @authors("max42")
    def test_queue_attributes(self):
        create("table", "//tmp/q", attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
        sync_mount_table("//tmp/q")

        assert get("//tmp/q/@queue_agent_stage") == "production"

        # Before queue is registered, queue agent backed attributes would throw resolution error.
        with raises_yt_error(code=yt_error_codes.ResolveErrorCode):
            get("//tmp/q/@queue_status")

        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "row_revision": YsonUint64(4567), "object_type": "table",
                      "dynamic": True, "sorted": False}])

        # Wait for queue status to become available.
        wait(lambda: get("//tmp/q/@queue_status/partition_count") == 1, ignore_exceptions=True)

        # Check the zeroth partition.

        def check_partition():
            partitions = get("//tmp/q/@queue_partitions")
            if len(partitions) == 1:
                assert partitions[0]["available_row_count"] == 0
                return True
            return False

        wait(check_partition)

        # Check that queue attributes are opaque.
        full_attributes = get("//tmp/q/@")
        for attribute in ("queue_status", "queue_partitions"):
            assert full_attributes[attribute] == YsonEntity()

    @authors("achulkov2")
    def test_consumer_attributes(self):
        create("table", "//tmp/q", attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
        sync_mount_table("//tmp/q")
        create("table", "//tmp/c", attributes={"dynamic": True, "schema": BIGRT_CONSUMER_TABLE_SCHEMA,
                                               "target_queue": "primary://tmp/q"})
        with raises_yt_error('Builtin attribute "vital_queue_consumer" cannot be set'):
            set("//tmp/c/@vital_queue_consumer", True)
        set("//tmp/c/@treat_as_queue_consumer", True)
        set("//tmp/c/@vital_queue_consumer", True)
        sync_mount_table("//tmp/c")

        assert get("//tmp/c/@queue_agent_stage") == "production"

        # Check that queue_agent_stage is writable.
        set("//tmp/c/@queue_agent_stage", "testing")
        set("//tmp/c/@queue_agent_stage", "production")

        # Before consumer is registered, queue agent backed attributes would throw resolution error.
        with raises_yt_error(code=yt_error_codes.ResolveErrorCode):
            get("//tmp/c/@queue_consumer_status")

        insert_rows("//sys/queue_agents/consumers",
                    [{"cluster": "primary", "path": "//tmp/c", "row_revision": YsonUint64(0)}])
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": "//tmp/q", "row_revision": YsonUint64(4567)}])

        # Wait for consumer status to become available.
        wait(lambda: get("//tmp/c/@queue_consumer_status/partition_count") == 1, ignore_exceptions=True)

        # TODO: Check the queue_consumer_partitions attribute once the corresponding queue_controller code is written.

        # Check that consumer attributes are opaque.
        full_attributes = get("//tmp/c/@")
        for attribute in ("queue_consumer_status", "queue_consumer_partitions"):
            assert full_attributes[attribute] == YsonEntity()

    @authors("max42")
    def test_queue_agent_stage(self):
        create("table", "//tmp/q", attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
        sync_mount_table("//tmp/q")

        assert get("//tmp/q/@queue_agent_stage") == "production"

        set("//sys/@config/queue_agent_server/default_queue_agent_stage", "another_default")
        assert get("//tmp/q/@queue_agent_stage") == "another_default"

        set("//tmp/q/@queue_agent_stage", "testing")
        assert get("//tmp/q/@queue_agent_stage") == "testing"

        # There is no queue agent with stage "testing", so accessing queue status would result in an error.
        with raises_yt_error('Queue agent stage "testing" is not found'):
            get("//tmp/q/@queue_status")

        tx = start_transaction()
        with raises_yt_error("Operation cannot be performed in transaction"):
            set("//tmp/q/@queue_agent_stage", "value_under_tx", tx=tx)

    @authors("max42")
    def test_non_queues(self):
        create("table", "//tmp/q_static",
               attributes={"schema": [{"name": "data", "type": "string"}]})
        create("table", "//tmp/q_sorted_dynamic",
               attributes={"dynamic": True, "schema": [{"name": "key", "type": "string", "sort_order": "ascending"},
                                                       {"name": "value", "type": "string"}]})
        create("replicated_table", "//tmp/q_ordered_replicated",
               attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
        queue_attributes = ["queue_status", "queue_partitions"]

        result = get("//tmp", attributes=queue_attributes)
        for name in ("q_static", "q_sorted_dynamic", "q_ordered_replicated"):
            assert not result[name].attributes
            for attribute in queue_attributes:
                assert not exists("//tmp/" + name + "/@" + attribute)

        dynamic_table_attributes = ["queue_agent_stage"]
        result = get("//tmp", attributes=dynamic_table_attributes)
        for name in ("q_static",):
            assert not result[name].attributes
            for attribute in dynamic_table_attributes:
                assert not exists("//tmp/" + name + "/@" + attribute)

    def _set_and_assert_revision_change(self, path, attribute, value, enable_revision_changing):
        old_revision = get(path + "/@attribute_revision")
        set(f"{path}/@{attribute}", value)
        assert get(f"{path}/@{attribute}") == value
        if enable_revision_changing:
            assert get(f"{path}/@attribute_revision") > old_revision
        else:
            assert get(f"{path}/@attribute_revision") == old_revision

    @authors("achulkov2")
    # COMPAT(kvk1920): Remove @enable_revision_changing_for_builtin_attributes from config.
    @pytest.mark.parametrize("enable_revision_changing", [False, True])
    def test_revision_changes_on_queue_attribute_change(self, enable_revision_changing):
        set("//sys/@config/cypress_manager/enable_revision_changing_for_builtin_attributes", enable_revision_changing)

        create("table", "//tmp/q", attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
        sync_mount_table("//tmp/q")

        self._set_and_assert_revision_change("//tmp/q", "queue_agent_stage", "testing", enable_revision_changing)

    @authors("achulkov2")
    # COMPAT(kvk1920): Remove @enable_revision_changing_for_builtin_attributes from config.
    @pytest.mark.parametrize("enable_revision_changing", [False, True])
    @pytest.mark.skipif(
        "BIGRT_CONSUMER_TABLE_SCHEMA" not in globals(),
        reason="BIGRT_CONSUMER_TABLE_SCHEMA doesn't exist supported in current version")
    def test_revision_changes_on_consumer_attribute_change(self, enable_revision_changing):
        set("//sys/@config/cypress_manager/enable_revision_changing_for_builtin_attributes", enable_revision_changing)

        create("table", "//tmp/q", attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
        sync_mount_table("//tmp/q")
        create("table", "//tmp/c", attributes={"dynamic": True, "schema": BIGRT_CONSUMER_TABLE_SCHEMA,  # noqa
                                               "target_queue": "primary://tmp/q"})

        self._set_and_assert_revision_change("//tmp/c", "treat_as_queue_consumer", True, enable_revision_changing)
        self._set_and_assert_revision_change("//tmp/c", "queue_agent_stage", "testing", enable_revision_changing)
        self._set_and_assert_revision_change("//tmp/c", "owner", "queue_agent", enable_revision_changing)
        self._set_and_assert_revision_change("//tmp/c", "vital_queue_consumer", True, enable_revision_changing)
        # NB: Changing user or custom attributes is already changes revision.
        self._set_and_assert_revision_change("//tmp/c", "target_queue", "haha:muahaha", enable_revision_changing=True)


class CypressSynchronizerOrchid(OrchidBase):
    @staticmethod
    def leader_orchid():
        agent_ids = ls("//sys/queue_agents/instances", verbose=False)
        for agent_id in agent_ids:
            if get("//sys/queue_agents/instances/{}/orchid/cypress_synchronizer/active".format(agent_id)):
                return CypressSynchronizerOrchid(agent_id)
        assert False, "No leading cypress synchronizer found"

    def get_poll_index(self):
        return get(self.queue_agent_orchid_path() + "/cypress_synchronizer/poll_index")

    def wait_fresh_poll(self):
        poll_index = self.get_poll_index()
        wait(lambda: self.get_poll_index() >= poll_index + 2)

    def get_poll_error(self):
        return YtError.from_dict(get(self.queue_agent_orchid_path() + "/cypress_synchronizer/poll_error"))


class TestCypressSynchronizerBase(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    def _get_queue_name(self, name):
        return "//tmp/q-{}".format(name)

    def _get_consumer_name(self, name):
        return "//tmp/c-{}".format(name)

    def _set_account_tablet_count_limit(self, account, value):
        set("//sys/accounts/{0}/@resource_limits/tablet_count".format(account), value)

    LAST_REVISIONS = dict()
    QUEUE_REGISTRY = []
    CONSUMER_REGISTRY = []

    def teardown_method(self, method):
        self._drop_queues()
        self._drop_consumers()

        super(TestCypressSynchronizerBase, self).teardown_method(method)

    def _create_queue_object(self, path, initiate_helpers=True, **queue_attributes):
        update_inplace(queue_attributes, {"dynamic": True, "schema": [{"name": "useless", "type": "string"}]})
        create("table",
               path,
               attributes=queue_attributes,
               force=True)
        sync_mount_table(path)
        if initiate_helpers:
            self.QUEUE_REGISTRY.append(path)
            assert path not in self.LAST_REVISIONS
            self.LAST_REVISIONS[path] = 0

    def _create_and_register_queue(self, path, **queue_attributes):
        insert_rows("//sys/queue_agents/queues",
                    [{"cluster": "primary", "path": path, "row_revision": YsonUint64(0)}])
        self._create_queue_object(path, **queue_attributes)
        assert self.LAST_REVISIONS[path] == 0

    def _drop_queues(self):
        for queue in self.QUEUE_REGISTRY:
            remove(queue, force=True)
            del self.LAST_REVISIONS[queue]
        self.QUEUE_REGISTRY.clear()

    def _create_consumer_object(self, path, initiate_helpers=True):
        create("table",
               path,
               attributes={"dynamic": True,
                           "schema": [{"name": "useless", "type": "string", "sort_order": "ascending"},
                                      {"name": "also_useless", "type": "string"}],
                           "treat_as_queue_consumer": True},
               force=True)
        if initiate_helpers:
            self.CONSUMER_REGISTRY.append(path)
            assert path not in self.LAST_REVISIONS
            self.LAST_REVISIONS[path] = 0

    def _create_and_register_consumer(self, path):
        insert_rows("//sys/queue_agents/consumers",
                    [{"cluster": "primary", "path": path, "row_revision": YsonUint64(0)}])
        self._create_consumer_object(path)
        assert self.LAST_REVISIONS[path] == 0

    def _drop_consumers(self):
        for consumer in self.CONSUMER_REGISTRY:
            remove(consumer, force=True)
            del self.LAST_REVISIONS[consumer]
        self.CONSUMER_REGISTRY.clear()

    def _check_queue_count_and_column_counts(self, queues, size):
        assert len(queues) == size
        column_counts = [len(row) for row in queues]
        assert column_counts == [len(init_queue_agent_state.QUEUE_TABLE_SCHEMA)] * size

    def _check_consumer_count_and_column_counts(self, consumers, size):
        assert len(consumers) == size
        column_counts = [len(row) for row in consumers]
        assert column_counts == [len(init_queue_agent_state.CONSUMER_TABLE_SCHEMA)] * size

    # expected_synchronization_errors should contain functions that assert for an expected error YSON
    def _get_queues_and_check_invariants(self, expected_count=None, expected_synchronization_errors=None):
        queues = select_rows("* from [//sys/queue_agents/queues]")
        if expected_count is not None:
            self._check_queue_count_and_column_counts(queues, expected_count)
        for queue in queues:
            if queue["synchronization_error"] != YsonEntity():
                synchronization_error = YtError.from_dict(queue["synchronization_error"])
                if expected_synchronization_errors is not None and queue["path"] in expected_synchronization_errors:
                    expected_synchronization_errors[queue["path"]](synchronization_error)
                    continue
                assert synchronization_error.code == 0
            assert queue["revision"] == get(queue["path"] + "/@attribute_revision")
        return queues

    # expected_synchronization_errors should contain functions that assert for an expected error YSON
    def _get_consumers_and_check_invariants(self, expected_count=None, expected_synchronization_errors=None):
        consumers = select_rows("* from [//sys/queue_agents/consumers]")
        if expected_count is not None:
            self._check_consumer_count_and_column_counts(consumers, expected_count)
        for consumer in consumers:
            if consumer["synchronization_error"] != YsonEntity():
                synchronization_error = YtError.from_dict(consumer["synchronization_error"])
                if expected_synchronization_errors is not None and consumer["path"] in expected_synchronization_errors:
                    expected_synchronization_errors[consumer["path"]](synchronization_error)
                    continue
                assert synchronization_error.code == 0
            assert consumer["revision"] == get(consumer["path"] + "/@attribute_revision")
            assert consumer["treat_as_queue_consumer"] == get(consumer["path"] + "/@treat_as_queue_consumer")
            # Enclosing into a list is a workaround for storing YSON with top-level attributes.
            assert consumer["schema"] == [get(consumer["path"] + "/@schema")]
            assert consumer["vital"] == get(
                consumer["path"] + "/@", attributes=["vital_queue_consumer"]).get("vital_queue_consumer", False)
        return consumers

    def _assert_constant_revision(self, row):
        assert self.LAST_REVISIONS[row["path"]] == row["row_revision"]

    def _assert_increased_revision(self, row):
        assert self.LAST_REVISIONS[row["path"]] < row["row_revision"]
        self.LAST_REVISIONS[row["path"]] = row["row_revision"]


class TestCypressSynchronizerCommon(TestCypressSynchronizerBase):
    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
        },
        "cypress_synchronizer": {
            "poll_period": 100,
            # For the watching version.
            "clusters": ["primary"],
        },
    }

    @authors("achulkov2")
    @pytest.mark.parametrize("policy", ["polling", "watching"])
    def test_alerts(self, policy):
        self._apply_dynamic_config_patch({
            "cypress_synchronizer": {
                "policy": policy
            }
        })

        orchid = CypressSynchronizerOrchid()
        alert_orchid = AlertManagerOrchid()

        q1 = self._get_queue_name("a")
        c1 = self._get_consumer_name("a")

        self._create_and_register_queue(q1)
        self._create_and_register_consumer(c1)
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=1)
        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        sync_unmount_table("//sys/queue_agents/queues")

        wait(lambda: "cypress_synchronizer_pass_failed" in alert_orchid.get_alerts())

    @authors("achulkov2")
    @pytest.mark.parametrize("policy", ["polling", "watching"])
    def test_no_alerts(self, policy):
        self._apply_dynamic_config_patch({
            "cypress_synchronizer": {
                "policy": policy
            }
        })

        orchid = CypressSynchronizerOrchid()
        alert_orchid = AlertManagerOrchid()

        q1 = self._get_queue_name("a")
        c1 = self._get_consumer_name("a")

        self._create_and_register_queue(q1)
        self._create_and_register_consumer(c1)
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=1)
        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        wait(lambda: not alert_orchid.get_alerts())


class TestCypressSynchronizerPolling(TestCypressSynchronizerBase):
    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
        },
        "cypress_synchronizer": {
            "poll_period": 100,
            "policy": "polling"
        },
    }

    @authors("achulkov2")
    def test_basic(self):
        orchid = CypressSynchronizerOrchid()

        q1 = self._get_queue_name("a")
        q2 = self._get_queue_name("b")
        c1 = self._get_consumer_name("a")
        c2 = self._get_consumer_name("b")

        self._create_and_register_queue(q1)
        self._create_and_register_consumer(c1)
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=1)
        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        self._create_and_register_queue(q2)
        self._create_and_register_consumer(c2)
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=2)
        consumers = self._get_consumers_and_check_invariants(expected_count=2)
        for queue in queues:
            if queue["path"] == q1:
                self._assert_constant_revision(queue)
            elif queue["path"] == q2:
                self._assert_increased_revision(queue)
        for consumer in consumers:
            if consumer["path"] == c1:
                self._assert_constant_revision(consumer)
            elif consumer["path"] == c2:
                self._assert_increased_revision(consumer)
                assert not consumer["vital"]

        set(c2 + "/@vital_queue_consumer", False)
        set(c2 + "/@target_queue", "a_cluster:a_path")
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=2)
        consumers = self._get_consumers_and_check_invariants(expected_count=2)
        for queue in queues:
            self._assert_constant_revision(queue)
        for consumer in consumers:
            if consumer["path"] == c1:
                self._assert_constant_revision(consumer)
            elif consumer["path"] == c2:
                self._assert_increased_revision(consumer)
                assert consumer["target_cluster"] == "a_cluster"
                assert consumer["target_path"] == "a_path"
                assert not consumer["vital"]

        sync_unmount_table("//sys/queue_agents/queues")
        orchid.wait_fresh_poll()
        assert_yt_error(orchid.get_poll_error(), yt_error_codes.TabletNotMounted)

        sync_mount_table("//sys/queue_agents/queues")
        set(c2 + "/@vital_queue_consumer", True)
        set(c2 + "/@target_queue", "another_cluster:another_path")
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=2)
        consumers = self._get_consumers_and_check_invariants(expected_count=2)
        for queue in queues:
            self._assert_constant_revision(queue)
        for consumer in consumers:
            if consumer["path"] == c1:
                self._assert_constant_revision(consumer)
            elif consumer["path"] == c2:
                self._assert_increased_revision(consumer)
                assert consumer["target_cluster"] == "another_cluster"
                assert consumer["target_path"] == "another_path"
                assert consumer["vital"]

    @authors("achulkov2")
    def test_content_change(self):
        orchid = CypressSynchronizerOrchid()

        q1 = self._get_queue_name("a")
        self._create_and_register_queue(q1, max_dynamic_store_row_count=1)

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        self._assert_increased_revision(queues[0])

        insert_rows(q1, [{"useless": "a"}])

        # Insert can fail while dynamic store is being flushed.
        def try_insert():
            insert_rows(q1, [{"useless": "a"}])
            return True
        wait(try_insert, ignore_exceptions=True)

        wait(lambda: len(get(q1 + "/@chunk_ids")) == 2)
        orchid.wait_fresh_poll()
        queues = self._get_queues_and_check_invariants(expected_count=1)
        # This checks that the revision doesn't change when dynamic stores are flushed.
        self._assert_constant_revision(queues[0])

    @authors("achulkov2")
    def test_synchronization_errors(self):
        orchid = CypressSynchronizerOrchid()

        q1 = self._get_queue_name("a")
        c1 = self._get_consumer_name("a")
        c2 = self._get_consumer_name("b")

        self._create_and_register_queue(q1)
        self._create_and_register_consumer(c1)

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=1)
        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        remove(q1)
        remove(c1)

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1, expected_synchronization_errors={
            q1: lambda error: assert_yt_error(error, yt_error_codes.ResolveErrorCode),
        })
        consumers = self._get_consumers_and_check_invariants(expected_count=1, expected_synchronization_errors={
            c1: lambda error: assert_yt_error(error, yt_error_codes.ResolveErrorCode),
        })

        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        self._create_queue_object(q1, initiate_helpers=False)
        self._create_consumer_object(c1, initiate_helpers=False)

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=1)
        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        self._create_and_register_consumer(c2)
        set(c1 + "/@target_queue", -1)
        set(c2 + "/@target_queue", "sugar_sugar:honey_honey")

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=2, expected_synchronization_errors={
            # This error has code 1, which is not very useful.
            c1: lambda error: assert_yt_error(error, 'Error parsing attribute "target_queue"')
        })

        for queue in queues:
            self._assert_constant_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)


class TestCypressSynchronizerWatching(TestCypressSynchronizerBase):
    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
        },
        "cypress_synchronizer": {
            "policy": "watching",
            "clusters": ["primary"],
            "poll_period": 100,
        },
    }

    @authors("achulkov2")
    def test_basic(self):
        orchid = CypressSynchronizerOrchid()

        q1 = self._get_queue_name("a")
        q2 = self._get_queue_name("b")
        c1 = self._get_consumer_name("a")
        c2 = self._get_consumer_name("b")

        self._create_queue_object(q1)
        self._create_consumer_object(c1)
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=1)
        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        self._create_queue_object(q2)
        self._create_consumer_object(c2)
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=2)
        consumers = self._get_consumers_and_check_invariants(expected_count=2)
        for queue in queues:
            if queue["path"] == q1:
                self._assert_constant_revision(queue)
            elif queue["path"] == q2:
                self._assert_increased_revision(queue)
        for consumer in consumers:
            if consumer["path"] == c1:
                self._assert_constant_revision(consumer)
            elif consumer["path"] == c2:
                self._assert_increased_revision(consumer)
                assert not consumer["vital"]

        set(c2 + "/@vital_queue_consumer", False)
        set(c2 + "/@target_queue", "a_cluster:a_path")
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=2)
        consumers = self._get_consumers_and_check_invariants(expected_count=2)
        for queue in queues:
            self._assert_constant_revision(queue)
        for consumer in consumers:
            if consumer["path"] == c1:
                self._assert_constant_revision(consumer)
            elif consumer["path"] == c2:
                self._assert_increased_revision(consumer)
                assert consumer["target_cluster"] == "a_cluster"
                assert consumer["target_path"] == "a_path"
                assert not consumer["vital"]

        sync_unmount_table("//sys/queue_agents/queues")
        orchid.wait_fresh_poll()
        assert_yt_error(orchid.get_poll_error(), yt_error_codes.TabletNotMounted)

        sync_mount_table("//sys/queue_agents/queues")
        set(c2 + "/@vital_queue_consumer", True)
        set(c2 + "/@target_queue", "another_cluster:another_path")
        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=2)
        consumers = self._get_consumers_and_check_invariants(expected_count=2)
        for queue in queues:
            self._assert_constant_revision(queue)
        for consumer in consumers:
            if consumer["path"] == c1:
                self._assert_constant_revision(consumer)
            elif consumer["path"] == c2:
                self._assert_increased_revision(consumer)
                assert consumer["target_cluster"] == "another_cluster"
                assert consumer["target_path"] == "another_path"
                assert consumer["vital"]

        set(c1 + "/@treat_as_queue_consumer", False)
        orchid.wait_fresh_poll()

        self._get_consumers_and_check_invariants(expected_count=1)

        remove(q2)
        orchid.wait_fresh_poll()

        self._get_queues_and_check_invariants(expected_count=1)

    # TODO(achulkov2): Unify this test with its copy once https://a.yandex-team.ru/review/2527564 is merged.
    @authors("achulkov2")
    def test_content_change(self):
        orchid = CypressSynchronizerOrchid()

        q1 = self._get_queue_name("a")
        self._create_queue_object(q1, max_dynamic_store_row_count=1)

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        self._assert_increased_revision(queues[0])

        insert_rows(q1, [{"useless": "a"}])

        # Insert can fail while dynamic store is being flushed.
        def try_insert():
            insert_rows(q1, [{"useless": "a"}])
            return True
        wait(try_insert, ignore_exceptions=True)

        wait(lambda: len(get(q1 + "/@chunk_ids")) == 2)
        orchid.wait_fresh_poll()
        queues = self._get_queues_and_check_invariants(expected_count=1)
        # This checks that the revision doesn't change when dynamic stores are flushed.
        self._assert_constant_revision(queues[0])

    @authors("achulkov2")
    def test_synchronization_errors(self):
        orchid = CypressSynchronizerOrchid()

        q1 = self._get_queue_name("a")
        c1 = self._get_consumer_name("a")
        c2 = self._get_consumer_name("b")

        self._create_queue_object(q1)
        self._create_consumer_object(c1)

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=1)
        for queue in queues:
            self._assert_increased_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)

        self._create_consumer_object(c2)
        set(c1 + "/@target_queue", -1)
        set(c2 + "/@target_queue", "sugar_sugar:honey_honey")

        orchid.wait_fresh_poll()

        queues = self._get_queues_and_check_invariants(expected_count=1)
        consumers = self._get_consumers_and_check_invariants(expected_count=2, expected_synchronization_errors={
            # This error has code 1, which is not very useful.
            c1: lambda error: assert_yt_error(error, 'Error parsing attribute "target_queue"')
        })

        for queue in queues:
            self._assert_constant_revision(queue)
        for consumer in consumers:
            self._assert_increased_revision(consumer)


class TestDynamicConfig(TestQueueAgentBase):
    NUM_QUEUE_AGENTS = 1

    DELTA_QUEUE_AGENT_DYNAMIC_CONFIG = {
        "queue_agent": {
            "poll_period": 100,
        },
        "cypress_synchronizer": {
            "poll_period": 100
        },
    }

    @authors("achulkov2")
    def test_basic(self):
        orchid = CypressSynchronizerOrchid()
        orchid.wait_fresh_poll()

        self._apply_dynamic_config_patch({
            "cypress_synchronizer": {
                "enable": False
            }
        })

        poll_index = orchid.get_poll_index()
        time.sleep(3)
        assert orchid.get_poll_index() == poll_index

        self._apply_dynamic_config_patch({
            "cypress_synchronizer": {
                "enable": True
            }
        })

        orchid.wait_fresh_poll()
