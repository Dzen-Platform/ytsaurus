import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *
from time import sleep
from yt.yson import YsonEntity
from yt.environment.helpers import assert_items_equal

##################################################################

class TestReplicatedDynamicTables(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 3
    NUM_SCHEDULERS = 0
    NUM_REMOTE_CLUSTERS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_NODE_CONFIG = {
        "cluster_connection": {
            # Disable cache
            "table_mount_cache": {
                "expire_after_successful_update_time": 0,
                "expire_after_failed_update_time": 0,
                "expire_after_access_time": 0,
                "refresh_time": 0
            },
            "timestamp_provider": {
                "update_period": 500,
            }
        },
        "tablet_node": {
            "tablet_manager": {
                "replicator_soft_backoff_time": 100
            }
        }
    }

    SIMPLE_SCHEMA = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value1", "type": "string"},
        {"name": "value2", "type": "int64"}
    ]

    PERTURBED_SCHEMA = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value2", "type": "int64"},
        {"name": "value1", "type": "string"}
    ]

    AGGREGATE_SCHEMA = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value1", "type": "string"},
        {"name": "value2", "type": "int64", "aggregate": "sum"}
    ]

    EXPRESSION_SCHEMA = [
        {"name": "hash", "type": "int64", "sort_order": "ascending", "expression": "key % 10"},
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value", "type": "int64"},
    ]

    REPLICA_CLUSTER_NAME = "remote_0"


    def setup(self):
        self.replica_driver = get_driver(cluster=self.REPLICA_CLUSTER_NAME)
        self.primary_driver = get_driver(cluster="primary")


    def _get_table_attributes(self, schema):
        return {
            "dynamic": True,
            "schema": schema
        }

    def _create_replicated_table(self, path, schema=SIMPLE_SCHEMA, attributes={}, mount=True):
        attributes.update(self._get_table_attributes(schema))
        attributes["enable_replication_logging"] = True
        create("replicated_table", path, attributes=attributes)
        if mount:
            self.sync_mount_table(path)

    def _create_replica_table(self, path, replica_id, schema=SIMPLE_SCHEMA, mount=True, replica_driver=None):
        if not replica_driver:
            replica_driver = self.replica_driver
        attributes = self._get_table_attributes(schema)
        attributes["upstream_replica_id"] = replica_id
        create("table", path, attributes=attributes, driver=replica_driver)
        if mount:
            self.sync_mount_table(path, driver=replica_driver)

    def _create_cells(self):
        self.sync_create_cells(1)
        self.sync_create_cells(1, driver=self.replica_driver)

    def _get_tablet_addresses(self, table):
        return [get("#%s/@peers/0/address" % tablet["cell_id"]) for tablet in get("//tmp/t/@tablets")]

    def _get_tablet_node_profiling_counter(self, node, counter_name):
        return get("//sys/nodes/%s/orchid/profiling/tablet_node/%s" % (node, counter_name))[-1]["value"]


    def test_replicated_table_must_be_dynamic(self):
        with pytest.raises(YtError): create("replicated_table", "//tmp/t")

    def test_simple(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        insert_rows("//tmp/t", [{"key": 1, "value1": "test"}], require_sync_replica=False)
        delete_rows("//tmp/t", [{"key": 2}], require_sync_replica=False)

    def test_replicated_tablet_node_profiling(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", attributes={"enable_profiling": True})

        insert_rows("//tmp/t", [{"key": 1, "value1": "test"}], require_sync_replica=False)

        sleep(2)

        addresses = self._get_tablet_addresses("//tmp/t")
        assert len(addresses) == 1

        def get_counter(counter_name):
            return self._get_tablet_node_profiling_counter(addresses[0], counter_name)

        def get_all_counters(count_name):
            return (
                get_counter("write/" + count_name),
                get_counter("commit/" + count_name))

        assert get_all_counters("row_count") == (1, 1)
        assert get_all_counters("data_weight") == (13, 13)

    def test_replica_tablet_node_profiling(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", attributes={"enable_profiling": True})
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id)

        addresses = self._get_tablet_addresses("//tmp/t")
        assert len(addresses) == 1

        def get_counter(counter_name):
            return self._get_tablet_node_profiling_counter(addresses[0], counter_name)

        def get_lag_row_count():
            return get_counter("replica/lag_row_count")

        def get_lag_time():
            return get_counter("replica/lag_time") / 1e6 # conversion from us to s

        self.sync_enable_table_replica(replica_id)
        sleep(2)

        assert get_lag_row_count() == 0
        assert get_lag_time() == 0

        insert_rows("//tmp/t", [{"key": 0, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(2)

        assert get_lag_row_count() == 0
        assert get_lag_time() == 0

        self.sync_unmount_table("//tmp/r", driver=self.replica_driver)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(2)

        assert get_lag_row_count() == 1
        assert 2 <= get_lag_time() <= 8

        insert_rows("//tmp/t", [{"key": 2, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(2)

        assert get_lag_row_count() == 2
        assert 4 <= get_lag_time() <= 10

        self.sync_mount_table("//tmp/r", driver=self.replica_driver)
        sleep(2)

        assert get_lag_row_count() == 0
        assert get_lag_time() == 0

    def test_replication_error(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id, mount=False)
        self.sync_enable_table_replica(replica_id)

        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 1
        tablet_id = tablets[0]["tablet_id"]

        def verify_error(message=None):
            errors = get("//tmp/t/@replicas/%s/errors" % replica_id)
            replica_table_tablets = get("#{0}/@tablets".format(replica_id))
            assert len(replica_table_tablets) == 1
            replica_table_tablet = replica_table_tablets[0]
            assert replica_table_tablet["tablet_id"] == tablet_id
            if len(errors) == 0:
                assert message == None
                assert "replication_error" not in replica_table_tablet
            else:
                assert len(errors) == 1
                assert errors[0]["message"] == message
                assert replica_table_tablet["replication_error"]["message"] == message
                assert tablet_id
                assert errors[0]["attributes"]["tablet_id"] == tablet_id

        verify_error()

        insert_rows("//tmp/t", [{"key": 1, "value1": "test1"}], require_sync_replica=False)
        sleep(1.0)

        verify_error("Table //tmp/r has no mounted tablets")

        self.sync_mount_table("//tmp/r", driver=self.replica_driver)
        sleep(1.0)

        verify_error()

    def test_replicated_in_memory_fail(self):
        self._create_cells()
        with pytest.raises(YtError):
            self._create_replicated_table("//tmp/t", attributes={"in_memory_mode": "compressed"})
        with pytest.raises(YtError):
            self._create_replicated_table("//tmp/t", attributes={"in_memory_mode": "uncompressed"})

    def test_add_replica_fail1(self):
        with pytest.raises(YtError): create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")

    def test_add_replica_fail2(self):
        create("table", "//tmp/t")
        with pytest.raises(YtError): create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")

    def test_add_replica_fail3(self):
        self._create_replicated_table("//tmp/t", mount=False)
        create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        with pytest.raises(YtError): create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")

    def test_add_remove_replica(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        assert exists("//tmp/t/@replicas/{0}".format(replica_id))
        attributes = get("#{0}/@".format(replica_id))
        assert attributes["type"] == "table_replica"
        assert attributes["state"] == "disabled"
        remove_table_replica(replica_id)
        assert not exists("#{0}/@".format(replica_id))

    def test_enable_disable_replica_unmounted(self):
        self._create_replicated_table("//tmp/t", mount=False)
        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "disabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][0]["state"] == "none"

        alter_table_replica(replica_id, enabled=True)
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "enabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][0]["state"] == "none"

        alter_table_replica(replica_id, enabled=False)
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "disabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][0]["state"] == "none"

    def test_enable_disable_replica_mounted(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        assert get("#{0}/@state".format(replica_id)) == "disabled"

        self.sync_enable_table_replica(replica_id)
        assert get("#{0}/@state".format(replica_id)) == "enabled"

        self.sync_disable_table_replica(replica_id)
        assert get("#{0}/@state".format(replica_id)) == "disabled"

    def test_in_sync_relicas_simple(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        assert get("#{0}/@state".format(replica_id)) == "disabled"

        self._create_replica_table("//tmp/r", replica_id)

        self.sync_enable_table_replica(replica_id)
        assert get("#{0}/@state".format(replica_id)) == "enabled"

        rows = [{"key": 0, "value1": "test", "value2": 42}]
        keys = [{"key": 0}]

        timestamp0 = generate_timestamp()
        assert get_in_sync_replicas("//tmp/t", [], timestamp=timestamp0) == [replica_id]

        sleep(1.0)  # wait for last timestamp update
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp0) == [replica_id]

        insert_rows("//tmp/t", rows, require_sync_replica=False)
        timestamp1 = generate_timestamp()
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp0) == [replica_id]
        sleep(1.0)  # wait for replica update
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp0) == [replica_id]
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp1) == [replica_id]

        timestamp2 = generate_timestamp()
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp0) == [replica_id]
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp1) == [replica_id]

        sleep(1.0)  # wait for last timestamp update
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp2) == [replica_id]
        assert get_in_sync_replicas("//tmp/t", [], timestamp=timestamp0) == [replica_id]

    def test_in_sync_relicas_expression(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.EXPRESSION_SCHEMA)
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id, schema=self.EXPRESSION_SCHEMA)

        self.sync_enable_table_replica(replica_id)

        timestamp0 = generate_timestamp()
        assert get_in_sync_replicas("//tmp/t", [], timestamp=timestamp0) == [replica_id]

        rows = [{"key": 1, "value": 2}]
        keys = [{"key": 1}]
        insert_rows("//tmp/t", rows, require_sync_replica=False)
        timestamp1 = generate_timestamp()
        sleep(1.0)  # wait for replica update
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"hash": 1, "key": 1, "value": 2}]
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp0) == [replica_id]
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp1) == [replica_id]

    def test_in_sync_relicas_disabled(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        replica_id1 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r1")
        replica_id2 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r2")

        self._create_replica_table("//tmp/r1", replica_id1)
        self._create_replica_table("//tmp/r2", replica_id2)

        self.sync_enable_table_replica(replica_id1)

        assert get("#{0}/@state".format(replica_id1)) == "enabled"
        assert get("#{0}/@state".format(replica_id2)) == "disabled"

        rows = [{"key": 0, "value1": "test", "value2": 42}]
        keys = [{"key": 0}]

        insert_rows("//tmp/t", rows, require_sync_replica=False)
        timestamp = generate_timestamp()

        assert_items_equal(get_in_sync_replicas("//tmp/t", [], timestamp=timestamp), [replica_id1, replica_id2])

        sleep(1.0)
        assert get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp) == [replica_id1]

        self.sync_enable_table_replica(replica_id2)
        sleep(1.0)
        assert_items_equal(get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp), [replica_id1, replica_id2])
        assert_items_equal(get_in_sync_replicas("//tmp/t", [], timestamp=timestamp), [replica_id1, replica_id2])

    def test_async_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id)

        self.sync_enable_table_replica(replica_id)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        insert_rows("//tmp/t", [{"key": 1, "value1": "new_test"}], update=True, require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 123}]

        insert_rows("//tmp/t", [{"key": 1, "value2": 456}], update=True, require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 456}]

        delete_rows("//tmp/t", [{"key": 1}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == []

    def test_sync_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id1 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r1", attributes={"mode": "sync"})
        replica_id2 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r2", attributes={"mode": "async"})
        self._create_replica_table("//tmp/r1", replica_id1)
        self._create_replica_table("//tmp/r2", replica_id2)
        self.sync_enable_table_replica(replica_id1)
        self.sync_enable_table_replica(replica_id2)

        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 1
        tablet_id = tablets[0]["tablet_id"]

        def _do():
            before_index1 = get("#{0}/@tablets/0/current_replication_row_index".format(replica_id1))
            before_index2 = get("#{0}/@tablets/0/current_replication_row_index".format(replica_id2))
            assert before_index1 == before_index2

            before_ts1 = get("#{0}/@tablets/0/current_replication_timestamp".format(replica_id1))
            before_ts2 = get("#{0}/@tablets/0/current_replication_timestamp".format(replica_id2))
            assert before_ts1 == before_ts2

            insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}])
            assert select_rows("* from [//tmp/r1]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]
            sleep(1.0)
            assert select_rows("* from [//tmp/r2]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

            insert_rows("//tmp/t", [{"key": 1, "value1": "new_test"}], update=True)
            assert select_rows("* from [//tmp/r1]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 123}]
            sleep(1.0)
            assert select_rows("* from [//tmp/r2]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 123}]

            insert_rows("//tmp/t", [{"key": 1, "value2": 456}], update=True)
            assert select_rows("* from [//tmp/r1]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 456}]
            sleep(1.0)
            assert select_rows("* from [//tmp/r2]", driver=self.replica_driver) == [{"key": 1, "value1": "new_test", "value2": 456}]

            delete_rows("//tmp/t", [{"key": 1}])
            assert select_rows("* from [//tmp/r1]", driver=self.replica_driver) == []
            sleep(1.0)
            assert select_rows("* from [//tmp/r2]", driver=self.replica_driver) == []

            after_index1 = get("#{0}/@tablets/0/current_replication_row_index".format(replica_id1))
            after_index2 = get("#{0}/@tablets/0/current_replication_row_index".format(replica_id2))
            assert after_index1 == after_index2
            assert after_index1 == before_index1 + 4

            after_ts1 = get("#{0}/@tablets/0/current_replication_timestamp".format(replica_id1))
            after_ts2 = get("#{0}/@tablets/0/current_replication_timestamp".format(replica_id2))
            assert after_ts1 == after_ts2
            assert after_ts1 > before_ts1

        _do()

        alter_table_replica(replica_id1, mode="async")
        alter_table_replica(replica_id1, mode="sync")

        sleep(1.0)

        _do()

    def test_cannot_sync_write_into_disabled_replica(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r", attributes={"mode": "sync"})
        self._create_replica_table("//tmp/r", replica_id)
        with pytest.raises(YtError): insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}])

    def test_upstream_replica_id_check1(self):
        self._create_cells()
        self._create_replica_table("//tmp/r", "1-2-3-4")
        with pytest.raises(YtError): insert_rows("//tmp/r", [{"key": 1, "value2": 456}], driver=self.replica_driver)

    def test_upstream_replica_id_check2(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r", attributes={"mode": "sync"})
        self._create_replica_table("//tmp/r", "1-2-3-4")
        self.sync_enable_table_replica(replica_id)
        with pytest.raises(YtError): insert_rows("//tmp/t", [{"key": 1, "value2": 456}])

    def test_wait_for_sync(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id1 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r1", attributes={"mode": "sync"})
        replica_id2 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r2", attributes={"mode": "async"})
        self._create_replica_table("//tmp/r1", replica_id1)
        self._create_replica_table("//tmp/r2", replica_id2)

        self.sync_enable_table_replica(replica_id1)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}])
        assert select_rows("* from [//tmp/r1]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]
        assert select_rows("* from [//tmp/r2]", driver=self.replica_driver) == []

        alter_table_replica(replica_id1, mode="async")
        alter_table_replica(replica_id2, mode="sync")

        sleep(1.0)

        with pytest.raises(YtError): insert_rows("//tmp/t", [{"key": 1, "value1": "test2", "value2": 456}])

        self.sync_enable_table_replica(replica_id2)

        sleep(1.0)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test2", "value2": 456}])
        assert select_rows("* from [//tmp/r2]", driver=self.replica_driver) == [{"key": 1, "value1": "test2", "value2": 456}]

        sleep(1.0)

        assert select_rows("* from [//tmp/r1]", driver=self.replica_driver) == [{"key": 1, "value1": "test2", "value2": 456}]

    def test_disable_propagates_replication_row_index(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id)

        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        self.sync_enable_table_replica(replica_id)

        assert get("#{0}/@tablets/0/current_replication_row_index".format(replica_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        self.sync_disable_table_replica(replica_id)

        assert get("#{0}/@tablets/0/current_replication_row_index".format(replica_id)) == 1

    def test_unmount_propagates_replication_row_index(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id)

        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        self.sync_enable_table_replica(replica_id)

        assert get("#{0}/@tablets/0/current_replication_row_index".format(replica_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        self.sync_unmount_table("//tmp/t")

        assert get("#{0}/@tablets/0/current_replication_row_index".format(replica_id)) == 1

    @pytest.mark.parametrize("with_data", [False, True])
    def test_start_replication_timestamp(self, with_data):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        insert_rows("//tmp/t", [{"key": 1, "value1": "test"}], require_sync_replica=False)

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r", attributes={
            "start_replication_timestamp": generate_timestamp()
        })
        self._create_replica_table("//tmp/r", replica_id)

        self.sync_enable_table_replica(replica_id)

        if with_data:
            insert_rows("//tmp/t", [{"key": 2, "value1": "test"}], require_sync_replica=False)

        sleep(1.0)

        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == \
            ([{"key": 2, "value1": "test", "value2": YsonEntity()}] if with_data else \
            [])

    @pytest.mark.parametrize("ttl, chunk_count, trimmed_row_count", [
        (0, 1, 1),
        (60000, 2, 0)
    ])
    def test_replication_trim(self, ttl, chunk_count, trimmed_row_count):
        self._create_cells()
        self._create_replicated_table("//tmp/t", attributes={"min_replication_log_ttl": ttl})
        self.sync_mount_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id)

        self.sync_enable_table_replica(replica_id)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test1"}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test1", "value2": YsonEntity()}]

        self.sync_unmount_table("//tmp/t")
        assert get("//tmp/t/@chunk_count") == 1
        assert get("//tmp/t/@tablets/0/flushed_row_count") == 1
        assert get("//tmp/t/@tablets/0/trimmed_row_count") == 0

        self.sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", [{"key": 2, "value1": "test2"}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test1", "value2": YsonEntity()}, {"key": 2, "value1": "test2", "value2": YsonEntity()}]
        self.sync_unmount_table("//tmp/t")

        assert get("//tmp/t/@chunk_count") == chunk_count
        assert get("//tmp/t/@tablets/0/flushed_row_count") == 2
        assert get("//tmp/t/@tablets/0/trimmed_row_count") == trimmed_row_count

    def test_aggregate_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.AGGREGATE_SCHEMA)
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id, schema=self.AGGREGATE_SCHEMA)

        self.sync_enable_table_replica(replica_id)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test1"}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test1", "value2": YsonEntity()}]

        insert_rows("//tmp/t", [{"key": 1, "value1": "test2", "value2": 100}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test2", "value2": 100}]

        insert_rows("//tmp/t", [{"key": 1, "value2": 50}], aggregate=True, update=True, require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test2", "value2": 150}]

    def test_replication_lag(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.AGGREGATE_SCHEMA)
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id, schema=self.AGGREGATE_SCHEMA)

        assert get("#{0}/@replication_lag_time".format(replica_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test1"}], require_sync_replica=False)
        sleep(1.0)
        get("#{0}/@replication_lag_time".format(replica_id)) > 1000000

        self.sync_enable_table_replica(replica_id)
        sleep(1.0)
        assert get("#{0}/@replication_lag_time".format(replica_id)) == 0

    def test_expression_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.EXPRESSION_SCHEMA)
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id, schema=self.EXPRESSION_SCHEMA)

        self.sync_enable_table_replica(replica_id)

        insert_rows("//tmp/t", [{"key": 1, "value": 2}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"hash": 1, "key": 1, "value": 2}]

        insert_rows("//tmp/t", [{"key": 12, "value": 12}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [
            {"hash": 1, "key": 1, "value": 2},
            {"hash": 2, "key": 12, "value": 12}]

        delete_rows("//tmp/t", [{"key": 1}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"hash": 2, "key": 12, "value": 12}]

    def test_shard_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.SIMPLE_SCHEMA, attributes={"pivot_keys": [[], [10]]})
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id, schema=self.SIMPLE_SCHEMA)

        self.sync_enable_table_replica(replica_id)

        insert_rows("//tmp/t", [{"key": 1, "value1": "v", "value2": 2}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "v", "value2": 2}]

        # ensuring that we have correctly populated data here
        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 2
        assert tablets[0]["index"] == 0
        assert tablets[0]["pivot_key"] == []
        assert tablets[1]["index"] == 1
        assert tablets[1]["pivot_key"] == [10]

    def test_reshard_replication(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.SIMPLE_SCHEMA, mount=False)
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id, schema=self.SIMPLE_SCHEMA)

        self.sync_enable_table_replica(replica_id)

        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 1

        reshard_table("//tmp/t", [[], [10]])
        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 2

        reshard_table("//tmp/t", [[], [10], [20]])
        tablets = get("//tmp/t/@tablets")

        # ensuring that we have correctly populated data here
        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 3
        assert tablets[0]["index"] == 0
        assert tablets[0]["pivot_key"] == []
        assert tablets[1]["index"] == 1
        assert tablets[1]["pivot_key"] == [10]
        assert tablets[2]["index"] == 2
        assert tablets[2]["pivot_key"] == [20]

        self.sync_mount_table("//tmp/t")
        self.sync_unmount_table("//tmp/t")

    def test_replica_ops_require_exclusive_lock(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", mount=False)

        tx1 = start_transaction()
        lock("//tmp/t", mode="exclusive", tx=tx1)
        with pytest.raises(YtError): create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        abort_transaction(tx1)

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        tx2 = start_transaction()
        lock("//tmp/t", mode="exclusive", tx=tx2)
        with pytest.raises(YtError): remove_table_replica(replica_id)

    def test_lookup(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.AGGREGATE_SCHEMA)
        replica_id1 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r1", attributes={"mode": "async"})
        replica_id2 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r2", attributes={"mode": "sync"})
        self._create_replica_table("//tmp/r1", replica_id1, schema=self.AGGREGATE_SCHEMA)
        self._create_replica_table("//tmp/r2", replica_id2, schema=self.AGGREGATE_SCHEMA)

        self.sync_enable_table_replica(replica_id1)
        self.sync_enable_table_replica(replica_id2)

        for i in xrange(10):
            insert_rows("//tmp/t", [{"key": i, "value1": "test" + str(i)}])

        for i in xrange(9):
            assert lookup_rows("//tmp/t", [{"key": i}, {"key": i + 1}], column_names=["key", "value1"]) == \
                               [{"key": i, "value1": "test" + str(i)}, {"key": i + 1, "value1": "test" + str(i + 1)}]

        assert lookup_rows("//tmp/t", [{"key": 100000}]) == []

        self.sync_disable_table_replica(replica_id2)
        alter_table_replica(replica_id2, mode="async")
        clear_metadata_caches()
        sleep(1.0)
        with pytest.raises(YtError): insert_rows("//tmp/t", [{"key": 666, "value1": "hello"}])
        insert_rows("//tmp/t", [{"key": 666, "value1": "hello"}], require_sync_replica=False)

        alter_table_replica(replica_id2, mode="sync")
        sleep(1.0)
        with pytest.raises(YtError): lookup_rows("//tmp/t", [{"key": 666}]) == []

        self.sync_enable_table_replica(replica_id2)
        sleep(1.0)
        assert lookup_rows("//tmp/t", [{"key": 666}], column_names=["key", "value1"]) == [{"key": 666, "value1": "hello"}]

        alter_table_replica(replica_id2, mode="async")
        sleep(1.0)
        with pytest.raises(YtError): lookup_rows("//tmp/t", [{"key": 666}]) == []

    def test_select(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t", schema=self.AGGREGATE_SCHEMA)
        replica_id1 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r1", attributes={"mode": "async"})
        replica_id2 = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r2", attributes={"mode": "sync"})
        self._create_replica_table("//tmp/r1", replica_id1, schema=self.AGGREGATE_SCHEMA)
        self._create_replica_table("//tmp/r2", replica_id2, schema=self.AGGREGATE_SCHEMA)

        self.sync_enable_table_replica(replica_id1)
        self.sync_enable_table_replica(replica_id2)

        rows = [{"key": i, "value1": "test" + str(i)} for i in xrange(10)]
        insert_rows("//tmp/t", rows)
        assert_items_equal(select_rows("key, value1 from [//tmp/t]"), rows)
        assert_items_equal(select_rows("sum(key) from [//tmp/t] group by 0"), [{"sum(key)": 45}])

        create("table", "//tmp/z", attributes={
            "dynamic": True,
            "schema": self.SIMPLE_SCHEMA
        })
        with pytest.raises(YtError): select_rows("* from [//tmp/t] as t1 join [//tmp/z] as t2 on t1.key = t2.key")

        alter_table_replica(replica_id2, mode="async")
        sleep(1.0)
        with pytest.raises(YtError): select_rows("* from [//tmp/t]")

    def test_local_sync_replica_yt_7571(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", "primary", "//tmp/r", attributes={"mode": "sync"})
        self._create_replica_table("//tmp/r", replica_id, replica_driver=self.primary_driver)
        self.sync_enable_table_replica(replica_id)

        rows = [{"key": i, "value1": "test" + str(i)} for i in xrange(10)]
        insert_rows("//tmp/t", rows)

        assert_items_equal(select_rows("key, value1 from [//tmp/t]", driver=self.primary_driver), rows)
        assert_items_equal(select_rows("key, value1 from [//tmp/r]", driver=self.primary_driver), rows)

    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_inverted_schema(self, mode):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r", attributes={"mode": mode})
        self._create_replica_table("//tmp/r", replica_id, schema=self.PERTURBED_SCHEMA)
        self.sync_enable_table_replica(replica_id)

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 10}], require_sync_replica=False)
        sleep(1.0)
        get("//tmp/t/@replicas")
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 10}]

        delete_rows("//tmp/t", [{"key": 1}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == []

##################################################################

class TestReplicatedDynamicTablesMulticell(TestReplicatedDynamicTables):
    NUM_SECONDARY_MASTER_CELLS = 2
