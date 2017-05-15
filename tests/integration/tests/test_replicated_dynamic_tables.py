import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *
from time import sleep
from yt.yson import YsonEntity

##################################################################

class TestReplicatedDynamicTables(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 3
    NUM_SCHEDULERS = 0
    NUM_REMOTE_CLUSTERS = 1

    DELTA_NODE_CONFIG = {
        "cluster_connection": {
            # Disable cache
            "table_mount_cache": {
                "expire_after_successful_update_time": 0,
                "expire_after_failed_update_time": 0,
                "expire_after_access_time": 0,
                "refresh_time": 0
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

    def _create_replica_table(self, path, replica_id, schema=SIMPLE_SCHEMA, mount=True):
        attributes = self._get_table_attributes(schema)
        attributes["upstream_replica_id"] = replica_id
        create("table", path, attributes=attributes, driver=self.replica_driver)
        if mount:
            self.sync_mount_table(path, driver=self.replica_driver)

    def _create_cells(self):
        self.sync_create_cells(1)
        self.sync_create_cells(1, driver=self.replica_driver)


    # def test_replication_mode_validation_on_create(self):
    #    with pytest.raises(YtError): create("table", "//tmp/t", attributes={
    #        "replication_mode": "source"
    #    })
    #    with pytest.raises(YtError): create("table", "//tmp/t", attributes={
    #        "replication_mode": "source",
    #        "schema": self.SIMPLE_SCHEMA
    #    })
    #    with pytest.raises(YtError): create("table", "//tmp/t", attributes={
    #        "replication_mode": "source",
    #        "dynamic": True,
    #        "schema": [
    #            {"name": "value1", "type": "string"},
    #            {"name": "value2", "type": "int64"}
    #        ]
    #    })

    # def test_default_replication_mode(self):
    #     create("table", "//tmp/t", attributes={
    #         "dynamic": True,
    #         "schema": self.SIMPLE_SCHEMA
    #     })
    #     assert get("//tmp/t/@replication_mode") == "none"
    #
    # def test_replication_mode_change_sanity(self):
    #    create("table", "//tmp/t", attributes={
    #        "dynamic": True,
    #        "schema": self.SIMPLE_SCHEMA
    #    })
    #    with pytest.raises(YtError): alter_table("//tmp/t", replication_mode="source")
    #    alter_table("//tmp/t", replication_mode="asynchronous_sink")
    #    assert get("//tmp/t/@replication_mode") == "asynchronous_sink"
    #    alter_table("//tmp/t", replication_mode="none")
    #    assert get("//tmp/t/@replication_mode") == "none"

    # def test_replication_mode_change_must_be_unmounted(self):
    #     self._create_cells()
    #     create("table", "//tmp/t", attributes={
    #         "dynamic": True,
    #         "schema": self.SIMPLE_SCHEMA
    #     })
    #     self.sync_mount_table("//tmp/t")
    #     with pytest.raises(YtError): alter_table("//tmp/t", replication_mode="asynchronous_sink")


    def test_replicated_table_must_be_dynamic(self):
        with pytest.raises(YtError): create("replicated_table", "//tmp/t")

    def test_simple(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        insert_rows("//tmp/t", [{"key": 1, "value1": "test"}], require_sync_replica=False)
        delete_rows("//tmp/t", [{"key": 2}], require_sync_replica=False)

    def test_replicated_in_memory_fail(self):
        self._create_cells()
        with pytest.raises(YtError):
            self._create_replicated_table("//tmp/t", attributes={"in_memory_mode": "compressed"})
        with pytest.raises(YtError):
            self._create_replicated_table("//tmp/t", attributes={"in_memory_mode": "uncompressed"})

    def test_replicated_in_memory_remount_fail(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        set("//tmp/t/@in_memory_mode", "compressed")
        with pytest.raises(YtError):
            remount_table("//tmp/t")

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
        assert attributes["tablets"][tablet_id]["state"] == "none"

        alter_table_replica(replica_id, enabled=True)
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "enabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][tablet_id]["state"] == "none"

        alter_table_replica(replica_id, enabled=False)
        attributes = get("#{0}/@".format(replica_id), attributes=["state", "tablets"])
        assert attributes["state"] == "disabled"
        assert len(attributes["tablets"]) == 1
        assert attributes["tablets"][tablet_id]["state"] == "none"

    def test_enable_disable_replica_mounted(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")

        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        assert get("#{0}/@state".format(replica_id)) == "disabled"

        self.sync_enable_table_replica(replica_id)
        assert get("#{0}/@state".format(replica_id)) == "enabled"

        self.sync_disable_table_replica(replica_id)
        assert get("#{0}/@state".format(replica_id)) == "disabled"

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
            before_index1 = get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id1, tablet_id))
            before_index2 = get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id2, tablet_id))
            assert before_index1 == before_index2

            before_ts1 = get("#{0}/@tablets/{1}/current_replication_timestamp".format(replica_id1, tablet_id))
            before_ts2 = get("#{0}/@tablets/{1}/current_replication_timestamp".format(replica_id2, tablet_id))
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

            after_index1 = get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id1, tablet_id))
            after_index2 = get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id2, tablet_id))
            assert after_index1 == after_index2
            assert after_index1 == before_index1 + 4

            after_ts1 = get("#{0}/@tablets/{1}/current_replication_timestamp".format(replica_id1, tablet_id))
            after_ts2 = get("#{0}/@tablets/{1}/current_replication_timestamp".format(replica_id2, tablet_id))
            assert after_ts1 == after_ts2
            assert after_ts1 > before_ts1

        _do()

        alter_table_replica(replica_id1, mode="async")
        alter_table_replica(replica_id1, mode="sync")

        sleep(1.0)

        _do()

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
   
        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        self.sync_disable_table_replica(replica_id)

        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 1

    def test_unmount_propagates_replication_row_index(self):
        self._create_cells()
        self._create_replicated_table("//tmp/t")
        replica_id = create_table_replica("//tmp/t", self.REPLICA_CLUSTER_NAME, "//tmp/r")
        self._create_replica_table("//tmp/r", replica_id)

        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        self.sync_enable_table_replica(replica_id)
   
        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 0

        insert_rows("//tmp/t", [{"key": 1, "value1": "test", "value2": 123}], require_sync_replica=False)
        sleep(1.0)
        assert select_rows("* from [//tmp/r]", driver=self.replica_driver) == [{"key": 1, "value1": "test", "value2": 123}]

        self.sync_unmount_table("//tmp/t")

        assert get("#{0}/@tablets/{1}/current_replication_row_index".format(replica_id, tablet_id)) == 1

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

##################################################################

class TestReplicatedDynamicTablesMulticell(TestReplicatedDynamicTables):
    NUM_SECONDARY_MASTER_CELLS = 2
