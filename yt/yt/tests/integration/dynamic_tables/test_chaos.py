from .test_dynamic_tables import DynamicTablesBase

from yt_env_setup import (
    Restarter,
    NODES_SERVICE,
    CHAOS_NODES_SERVICE,
    RPC_PROXIES_SERVICE,
)

from yt_commands import (
    authors, print_debug, wait, execute_command, get_driver, create_user, make_ace, check_permission,
    get, set, ls, create, exists, remove, copy, start_transaction, commit_transaction,
    sync_create_cells, sync_mount_table, sync_unmount_table, sync_flush_table,
    suspend_coordinator, resume_coordinator, reshard_table, alter_table, remount_table,
    insert_rows, delete_rows, lookup_rows, select_rows, pull_rows, trim_rows, lock_rows,
    create_replication_card, create_chaos_table_replica, alter_table_replica,
    build_snapshot, wait_for_cells, wait_for_chaos_cell, create_chaos_area,
    sync_create_chaos_cell, create_chaos_cell_bundle, generate_chaos_cell_id,
    align_chaos_cell_tag, migrate_replication_cards, alter_replication_card,
    get_in_sync_replicas, generate_timestamp, MaxTimestamp, raises_yt_error,
    create_table_replica, sync_enable_table_replica, get_tablet_infos)

from yt.environment.helpers import assert_items_equal
from yt.common import YtError

import yt.yson as yson

from yt_driver_bindings import Driver

import pytest
import time
from copy import deepcopy
from itertools import zip_longest

import builtins

##################################################################


class ChaosTestBase(DynamicTablesBase):
    NUM_CLOCKS = 1
    NUM_MASTER_CACHES = 1
    NUM_NODES = 5
    NUM_CHAOS_NODES = 1

    # TODO(gepardo): Re-enable native auth when YT-17837 is complete.
    USE_NATIVE_AUTH = False

    class CellsDisabled():
        def __init__(self, clusters=[], tablet_bundles=[], chaos_bundles=[]):
            self._clusters = clusters
            self._tablet_bundles = tablet_bundles
            self._chaos_bundles = chaos_bundles

        def __enter__(self):
            self._set_tag_filters("invalid")
            self._wait_for_cells("failed")

        def __exit__(self, exc_type, exception, traceback):
            self._set_tag_filters("")
            self._wait_for_cells("good")

        def _set_tag_filters(self, tag_filter):
            for cluster in self._clusters:
                driver = get_driver(cluster=cluster)
                for bundle in self._tablet_bundles:
                    set("//sys/tablet_cell_bundles/{0}/@node_tag_filter".format(bundle), tag_filter, driver=driver)
                for bundle in self._chaos_bundles:
                    set("//sys/chaos_cell_bundles/{0}/@node_tag_filter".format(bundle), tag_filter, driver=driver)

        def _wait_for_cells(self, state):
            for cluster in self._clusters:
                driver = get_driver(cluster=cluster)
                for bundle in self._tablet_bundles:
                    wait(lambda: get("//sys/tablet_cell_bundles/{0}/@health".format(bundle), driver=driver) == state)
                for bundle in self._chaos_bundles:
                    for cell in get("//sys/chaos_cell_bundles/{0}/@tablet_cell_ids".format(bundle), driver=driver):
                        target = ["good"] if state == "good" else ["failed", "degraded"]
                        wait(lambda: get("#{0}/@local_health".format(cell), driver=driver) in target)

    def _get_drivers(self):
        return [get_driver(cluster=cluster_name) for cluster_name in self.get_cluster_names()]

    def _create_chaos_cell_bundle(self, name="c", peer_cluster_names=None, meta_cluster_names=[], clock_cluster_tag=None):
        if peer_cluster_names is None:
            peer_cluster_names = self.get_cluster_names()
        return create_chaos_cell_bundle(
            name,
            peer_cluster_names,
            meta_cluster_names=meta_cluster_names,
            clock_cluster_tag=clock_cluster_tag)

    def _sync_create_chaos_cell(self, name="c", peer_cluster_names=None, meta_cluster_names=[], area="default"):
        if peer_cluster_names is None:
            peer_cluster_names = self.get_cluster_names()
        cell_id = generate_chaos_cell_id()
        sync_create_chaos_cell(name, cell_id, peer_cluster_names, meta_cluster_names=meta_cluster_names, area=area)
        return cell_id

    def _sync_create_chaos_bundle_and_cell(self, name="c", peer_cluster_names=None, meta_cluster_names=[], clock_cluster_tag=None):
        if peer_cluster_names is None:
            peer_cluster_names = self.get_cluster_names()
        self._create_chaos_cell_bundle(
            name=name,
            peer_cluster_names=peer_cluster_names,
            meta_cluster_names=meta_cluster_names,
            clock_cluster_tag=clock_cluster_tag)
        return self._sync_create_chaos_cell(
            name=name,
            peer_cluster_names=peer_cluster_names,
            meta_cluster_names=meta_cluster_names)

    def _list_chaos_nodes(self, driver=None):
        nodes = ls("//sys/cluster_nodes", attributes=["state", "flavors"], driver=driver)
        return [node for node in nodes if ("chaos" in node.attributes["flavors"])]

    def _get_table_orchids(self, path, driver=None):
        tablets = get("{0}/@tablets".format(path), driver=driver)
        tablet_ids = [tablet["tablet_id"] for tablet in tablets]
        orchids = [get("#{0}/orchid".format(tablet_id), driver=driver) for tablet_id in tablet_ids]
        return orchids

    def _wait_for_era(self, path, era=1, check_write=False, driver=None):
        import logging
        logger = logging.getLogger()

        def _check():
            for orchid in self._get_table_orchids(path, driver=driver):
                if not orchid["replication_card"] or orchid["replication_card"]["era"] != era:
                    logger.debug("Waiting {0} for era {1} got {2}".format(path, era, orchid["replication_card"]["era"] if orchid["replication_card"] else None))
                    return False
                if check_write and orchid["write_mode"] != "direct":
                    logger.debug("Waiting {0} for direct write mode but got {1}".format(path, orchid["write_mode"]))
                    return False
            return True
        wait(_check)

    def _sync_replication_card(self, card_id):
        def _check():
            card_replicas = get("#{0}/@replicas".format(card_id))
            return all(replica["state"] in ["enabled", "disabled"] and replica["mode"] in ["sync", "async"] for replica in card_replicas.values())
        wait(_check)
        return get("#{0}/@".format(card_id))

    def _create_chaos_table_replica(self, replica, **kwargs):
        attributes = {
            "enabled": replica.get("enabled", False)
        }
        for key in ["content_type", "mode", "replication_card_id", "table_path", "catchup", "replication_progress", "enable_replicated_table_tracker"]:
            if key in replica:
                attributes[key] = replica[key]
            if kwargs.get(key, None) is not None:
                attributes[key] = kwargs[key]
        return create_chaos_table_replica(replica["cluster_name"], replica["replica_path"], attributes=attributes)

    def _create_chaos_table_replicas(self, replicas, replication_card_id=None, table_path=None):
        return [self._create_chaos_table_replica(replica, replication_card_id=replication_card_id, table_path=table_path) for replica in replicas]

    def _prepare_replica_tables(self, replicas, replica_ids, create_tablet_cells=True, mount_tables=True):
        for replica, replica_id in zip(replicas, replica_ids):
            path = replica["replica_path"]
            driver = get_driver(cluster=replica["cluster_name"])
            alter_table(path, upstream_replica_id=replica_id, driver=driver)
            if create_tablet_cells:
                sync_create_cells(1, driver=driver)
            if mount_tables:
                sync_mount_table(path, driver=driver)

    def _create_replica_tables(self, replicas, replica_ids, create_tablet_cells=True, mount_tables=True, ordered=False, schema=None, pivot_keys=None, replication_progress=None):
        for replica, replica_id in zip(replicas, replica_ids):
            path = replica["replica_path"]
            driver = get_driver(cluster=replica["cluster_name"])
            create_table = self._create_queue_table
            if ordered:
                create_table = self._create_ordered_table
            elif replica["content_type"] == "data":
                create_table = self._create_sorted_table
            kwargs = {"driver": driver, "upstream_replica_id": replica_id}
            if schema:
                kwargs["schema"] = schema
            if pivot_keys:
                kwargs["pivot_keys"] = pivot_keys
            if replication_progress:
                kwargs["replication_progress"] = replication_progress
            create_table(path, **kwargs)
        self._prepare_replica_tables(replicas, replica_ids, create_tablet_cells=create_tablet_cells, mount_tables=mount_tables)

    def _sync_replication_era(self, card_id, replicas=None):
        replication_card = self._sync_replication_card(card_id)
        if not replicas:
            replicas = replication_card["replicas"].values()

        def _enabled(replica):
            return replica["enabled"] if "enabled" in replica else (replica["state"] in ["enabled", "enabling"])
        for replica in replicas:
            path = replica["replica_path"]
            driver = get_driver(cluster=replica["cluster_name"])
            check_write = replica["mode"] == "sync" and _enabled(replica)
            self._wait_for_era(path, era=replication_card["era"], check_write=check_write, driver=driver)

    def _create_chaos_tables(self, cell_id, replicas, sync_replication_era=True, create_replica_tables=True, create_tablet_cells=True, mount_tables=True, ordered=False, schema=None):
        card_id = create_replication_card(chaos_cell_id=cell_id)
        replica_ids = self._create_chaos_table_replicas(replicas, replication_card_id=card_id)
        if create_replica_tables:
            self._create_replica_tables(replicas, replica_ids, create_tablet_cells, mount_tables, ordered, schema=schema)
        if sync_replication_era:
            self._sync_replication_era(card_id, replicas)
        return card_id, replica_ids

    def _sync_alter_replica(self, card_id, replicas, replica_ids, replica_index, **kwargs):
        replica_id = replica_ids[replica_index]
        replica = replicas[replica_index]
        replica_driver=get_driver(cluster=replica["cluster_name"])
        alter_table_replica(replica_id, **kwargs)

        enabled = kwargs.get("enabled", None)
        mode = kwargs.get("mode", None)

        def _replica_checker(replica_info):
            if enabled is not None and replica_info["state"] != ("enabled" if enabled else "disabled"):
                return False
            if mode is not None and replica_info["mode"] != mode:
                return False
            return True

        def _check():
            orchids = self._get_table_orchids(replica["replica_path"], driver=replica_driver)
            if not any(orchid["replication_card"] for orchid in orchids):
                return False
            if not all(_replica_checker(orchid["replication_card"]["replicas"][replica_id]) for orchid in orchids):
                return False
            if len(builtins.set(orchid["replication_card"]["era"] for orchid in orchids)) > 1:
                return False
            replica_info = orchids[0]["replication_card"]["replicas"][replica_id]
            if replica_info["mode"] == "sync" and replica_info["state"] == "enabled":
                if not all(orchid["write_mode"] == "direct" for orchid in orchids):
                    return False
            return True

        wait(_check)
        era = self._get_table_orchids(replica["replica_path"], driver=replica_driver)[0]["replication_card"]["era"]

        # These request also includes coordinators to use same replication card cache key as insert_rows does.
        wait(lambda: get("#{0}/@era".format(card_id)) == era)
        replication_card = get("#{0}/@".format(card_id))
        assert replication_card["era"] == era

        def _check_sync():
            for replica in replication_card["replicas"].values():
                if replica["mode"] != "sync" or replica["state"] != "enabled":
                    continue
                orchids = self._get_table_orchids(replica["replica_path"], driver=get_driver(cluster=replica["cluster_name"]))
                if not all(orchid["replication_card"]["era"] == era for orchid in orchids):
                    return False
            return True

        wait(_check_sync)

    def _update_mount_config(self, attributes):
        if "mount_config" not in attributes:
            attributes["mount_config"] = {}
        if "replication_progress_update_tick_period" not in attributes["mount_config"]:
            attributes["mount_config"]["replication_progress_update_tick_period"] = 100

    def _create_sorted_table(self, path, **attributes):
        self._update_mount_config(attributes)
        super(ChaosTestBase, self)._create_sorted_table(path, **attributes)

    def _create_ordered_table(self, path, **attributes):
        self._update_mount_config(attributes)
        if "schema" not in attributes:
            attributes.update(
                {
                    "schema": [
                        {"name": "$timestamp", "type": "uint64"},
                        {"name": "key", "type": "int64"},
                        {"name": "value", "type": "string"},
                    ]
                }
            )
        if "commit_ordering" not in attributes:
            attributes.update({"commit_ordering": "strong"})
        super(ChaosTestBase, self)._create_ordered_table(path, **attributes)

    def _create_queue_table(self, path, **attributes):
        attributes.update({"dynamic": True})
        if "schema" not in attributes:
            attributes.update({
                "schema": [
                    {"name": "key", "type": "int64", "sort_order": "ascending"},
                    {"name": "value", "type": "string"}]
                })
        driver = attributes.pop("driver", None)
        create("replication_log_table", path, attributes=attributes, driver=driver)

    def _insistent_insert_rows(self, table, rows, driver=None):
        def _do():
            try:
                insert_rows(table, rows, driver=driver)
                return True
            except YtError as err:
                print_debug("Insert into {0} failed: ".format(table), err)
                return False
        wait(_do)

    def _init_replicated_table_tracker(self):
        for driver in self._get_drivers():
            set("//sys/@config/tablet_manager/replicated_table_tracker/use_new_replicated_table_tracker", True, driver=driver)
            set("//sys/@config/tablet_manager/replicated_table_tracker/bundle_health_cache", {
                "expire_after_successful_update_time": 100,
                "expire_after_failed_update_time": 100,
                "expire_after_access_time": 100,
                "refresh_time": 50,
            },
            driver=driver)

    def setup_method(self, method):
        super(ChaosTestBase, self).setup_method(method)

        # TODO(babenko): consider moving to yt_env_setup.py
        for driver in self._get_drivers():
            synchronizer_config = {
                "enable": True,
                "sync_period": 100,
                "full_sync_period": 200,
            }
            set("//sys/@config/chaos_manager/alien_cell_synchronizer", synchronizer_config, driver=driver)

            discovery_config = {
                "peer_count": 1,
                "update_period": 100,
                "node_tag_filter": "master_cache"
            }
            set("//sys/@config/node_tracker/master_cache_manager", discovery_config, driver=driver)

            chaos_nodes = self._list_chaos_nodes(driver)
            for chaos_node in chaos_nodes:
                set("//sys/cluster_nodes/{0}/@user_tags/end".format(chaos_node), "chaos_cache", driver=driver)

    def _get_schemas_by_name(self, schema_names):
        schemas = {
            "sorted_simple": [
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "string"},
            ],
            "sorted_value2": [
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "string"},
                {"name": "value2", "type": "string"},
            ],
            "sorted_key2": [
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "key2", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "string"},
            ],
            "sorted_key2_inverted": [
                {"name": "key2", "type": "int64", "sort_order": "ascending"},
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "string"},
            ],
            "sorted_hash": [
                {"name": "hash", "type": "uint64", "sort_order": "ascending", "expression": "farm_hash(key)"},
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "string"},
            ],
            "ordered_simple": [
                {"name": "$timestamp", "type": "uint64"},
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"},
            ],
            "ordered_value2": [
                {"name": "$timestamp", "type": "uint64"},
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"},
                {"name": "value2", "type": "string"},
            ],
            "ordered_simple_int": [
                {"name": "$timestamp", "type": "uint64"},
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "int64"},
            ],
        }
        return [schemas[name] for name in schema_names]


##################################################################


class TestChaos(ChaosTestBase):
    NUM_REMOTE_CLUSTERS = 2
    NUM_TEST_PARTITIONS = 20

    def setup_method(self, method):
        super(TestChaos, self).setup_method(method)

        primary_cell_tag = get("//sys/@primary_cell_tag")
        for driver in self._get_drivers():
            set("//sys/tablet_cell_bundles/default/@options/clock_cluster_tag", primary_cell_tag, driver=driver)

    @authors("savrus")
    def test_virtual_maps(self):
        tablet_cell_id = sync_create_cells(1)[0]
        tablet_bundle_id = get("//sys/tablet_cell_bundles/default/@id")

        chaos_bundle_ids = self._create_chaos_cell_bundle(name="default")

        assert tablet_bundle_id not in chaos_bundle_ids
        assert get("//sys/chaos_cell_bundles/default/@id") in chaos_bundle_ids

        chaos_cell_id = generate_chaos_cell_id()
        sync_create_chaos_cell("default", chaos_cell_id, self.get_cluster_names())

        assert_items_equal(get("//sys/chaos_cell_bundles"), ["default"])
        assert_items_equal(get("//sys/tablet_cell_bundles"), ["default", "sequoia"])
        assert_items_equal(get("//sys/tablet_cells"), [tablet_cell_id])
        assert_items_equal(get("//sys/chaos_cells"), [chaos_cell_id])

    @authors("savrus")
    def test_bundle_bad_options(self):
        params = {
            "type": "chaos_cell_bundle",
            "attributes": {
                "name": "chaos_bundle",
            }
        }
        with pytest.raises(YtError):
            execute_command("create", params)
        params["attributes"]["chaos_options"] = {"peers": [{"remote": False}]}
        with pytest.raises(YtError):
            execute_command("create", params)
        params["attributes"]["options"] = {
            "peer_count": 1,
            "changelog_account": "sys",
            "snapshot_account": "sys",
        }
        execute_command("create", params)
        with pytest.raises(YtError):
            set("//sys/chaos_cell_bundles/chaos_bundle/@options/independent_peers", False)

    @authors("babenko")
    def test_create_chaos_cell_with_duplicate_id(self):
        self._create_chaos_cell_bundle()

        cell_id = generate_chaos_cell_id()
        sync_create_chaos_cell("c", cell_id, self.get_cluster_names())

        with pytest.raises(YtError):
            sync_create_chaos_cell("c", cell_id, self.get_cluster_names())

    @authors("babenko")
    def test_create_chaos_cell_with_duplicate_tag(self):
        self._create_chaos_cell_bundle()

        cell_id = generate_chaos_cell_id()
        sync_create_chaos_cell("c", cell_id, self.get_cluster_names())

        cell_id_parts = cell_id.split("-")
        cell_id_parts[0], cell_id_parts[1] = cell_id_parts[1], cell_id_parts[0]
        another_cell_id = "-".join(cell_id_parts)

        with pytest.raises(YtError):
            sync_create_chaos_cell("c", another_cell_id, self.get_cluster_names())

    @authors("babenko")
    def test_create_chaos_cell_with_malformed_id(self):
        self._create_chaos_cell_bundle()
        with pytest.raises(YtError):
            sync_create_chaos_cell("c", "abcdabcd-fedcfedc-4204b1-12345678", self.get_cluster_names())

    @authors("savrus")
    def test_chaos_cells(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        def _check(peers, local_index=0):
            assert len(peers) == 3
            assert all(peer["address"] for peer in peers)
            assert sum("alien" in peer for peer in peers) == 2
            assert "alien" not in peers[local_index]

        _check(get("#{0}/@peers".format(cell_id)))

        for index, driver in enumerate(self._get_drivers()):
            _check(get("#{0}/@peers".format(cell_id), driver=driver), index)

    @authors("savrus")
    def test_remove_chaos_cell(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        remove("#{0}".format(cell_id))
        wait(lambda: not exists("#{0}".format(cell_id)))

        _, remote_driver0, _ = self._get_drivers()
        wait(lambda: not exists("#{0}/@peers/0/address".format(cell_id), driver=remote_driver0))

    @authors("savrus")
    def test_resurrect_chaos_cell(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)

        for driver in self._get_drivers():
            remove("#{0}".format(cell_id), driver=driver)
            wait(lambda: not exists("#{0}".format(cell_id), driver=driver))

        _, remote_driver0, _ = self._get_drivers()
        sync_unmount_table("//tmp/t")
        sync_unmount_table("//tmp/r0", driver=remote_driver0)

        peer_cluster_names = self.get_cluster_names()
        sync_create_chaos_cell("c", cell_id, peer_cluster_names)
        card_id = create_replication_card(chaos_cell_id=cell_id)
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, create_replica_tables=False, sync_replication_era=False)

        alter_table("//tmp/t", upstream_replica_id=replica_ids[0])
        alter_table("//tmp/r0", upstream_replica_id=replica_ids[1], driver=remote_driver0)
        sync_mount_table("//tmp/t")
        sync_mount_table("//tmp/r0", driver=remote_driver0)

        self._sync_replication_era(card_id, replicas)

        values = [{"key": 0, "value": "1"}]
        insert_rows("//tmp/t", values)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)

    @authors("savrus")
    def test_chaos_cell_update_acl(self):
        cell_id = self._sync_create_chaos_bundle_and_cell(name="chaos_bundle")
        create_user("u")
        set("//sys/chaos_cell_bundles/chaos_bundle/@options/snapshot_acl", [make_ace("allow", "u", "read")])
        assert check_permission("u", "read", "//sys/chaos_cells/{0}/0/snapshots".format(cell_id))["action"] == "allow"

    @authors("savrus")
    def test_replication_card(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        card_id = create_replication_card(chaos_cell_id=cell_id)
        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "replica_path": "//tmp/r1"}
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, replication_card_id=card_id)

        card = get("#{0}/@".format(card_id))
        card_replicas = [{key: r[key] for key in list(replicas[0].keys())} for r in card["replicas"].values()]
        assert_items_equal(card_replicas, replicas)
        assert card["id"] == card_id
        assert card["type"] == "replication_card"
        card_replicas = [{key: r[key] for key in list(replicas[0].keys())} for r in card["replicas"].values()]
        assert_items_equal(card_replicas, replicas)

        assert get("#{0}/@id".format(card_id)) == card_id
        assert get("#{0}/@type".format(card_id)) == "replication_card"

        card_attributes = ls("#{0}/@".format(card_id))
        assert "id" in card_attributes
        assert "type" in card_attributes
        assert "era" in card_attributes
        assert "replicas" in card_attributes
        assert "coordinator_cell_ids" in card_attributes

        for replica_index, replica_id in enumerate(replica_ids):
            replica = get("#{0}/@".format(replica_id))
            assert replica["id"] == replica_id
            assert replica["type"] == "chaos_table_replica"
            assert replica["replication_card_id"] == card_id
            assert replica["cluster_name"] == replicas[replica_index]["cluster_name"]
            assert replica["replica_path"] == replicas[replica_index]["replica_path"]

        assert exists("#{0}".format(card_id))
        assert exists("#{0}".format(replica_id))
        assert exists("#{0}/@id".format(replica_id))
        assert not exists("#{0}/@nonexisting".format(replica_id))

        remove("#{0}".format(card_id))
        assert not exists("#{0}".format(card_id))

    @authors("savrus")
    def test_chaos_table(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"}
        ]
        self._create_chaos_tables(cell_id, replicas)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)

        assert lookup_rows("//tmp/t", [{"key": 0}]) == values
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 0}], driver=remote_driver1) == values)

    @authors("savrus")
    def test_pull_rows(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": False, "replica_path": "//tmp/t"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        def _pull_rows(progress_timestamp):
            return pull_rows(
                "//tmp/q",
                replication_progress={
                    "segments": [{"lower_key": [], "timestamp": progress_timestamp}],
                    "upper_key": [yson.to_yson_type(None, attributes={"type": "max"})]
                },
                upstream_replica_id=replica_ids[0])

        def _sync_pull_rows(progress_timestamp):
            wait(lambda: len(_pull_rows(progress_timestamp)) == 1)
            result = _pull_rows(progress_timestamp)
            assert len(result) == 1
            return result[0]

        def _sync_check(progress_timestamp, expected_row):
            row = _sync_pull_rows(progress_timestamp)
            assert row["key"] == expected_row["key"]
            assert str(row["value"][0]) == expected_row["value"]
            return row.attributes["write_timestamps"][0]

        insert_rows("//tmp/q", [{"key": 0, "value": "0"}])
        timestamp1 = _sync_check(0, {"key": 0, "value": "0"})

        insert_rows("//tmp/q", [{"key": 1, "value": "1"}])
        timestamp2 = _sync_check(timestamp1, {"key": 1, "value": "1"})

        delete_rows("//tmp/q", [{"key": 0}])
        row = _sync_pull_rows(timestamp2)
        assert row["key"] == 0
        assert len(row.attributes["write_timestamps"]) == 0
        assert len(row.attributes["delete_timestamps"]) == 1

    @authors("savrus")
    def test_serialized_pull_rows(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": False, "replica_path": "//tmp/t"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False)
        reshard_table("//tmp/q", [[], [1], [2], [3], [4]])
        sync_mount_table("//tmp/q")
        self._sync_replication_era(card_id, replicas[:1])

        for i in (1, 2, 0, 4, 3):
            insert_rows("//tmp/q", [{"key": i, "value": str(i)}])

        def _pull_rows():
            return pull_rows(
                "//tmp/q",
                replication_progress={
                    "segments": [{"lower_key": [], "timestamp": 0}],
                    "upper_key": [yson.to_yson_type(None, attributes={"type": "max"})]
                },
                upstream_replica_id=replica_ids[0],
                order_rows_by_timestamp=True)

        wait(lambda: len(_pull_rows()) == 5)

        result = _pull_rows()
        timestamps = [row.attributes["write_timestamps"][0] for row in result]
        import logging
        logger = logging.getLogger()
        logger.debug("Write timestamps: {0}".format(timestamps))

        for i in range(len(timestamps) - 1):
            assert timestamps[i] < timestamps[i+1], "Write timestamp order mismatch for positions {0} and {1}: {2} > {3}".format(i, i+1, timestamps[i], timestamps[i+1])

    @authors("savrus")
    @pytest.mark.parametrize("reshard", [True, False])
    def test_advanced_pull_rows(self, reshard):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": False, "replica_path": "//tmp/t"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False)
        if reshard:
            reshard_table("//tmp/q", [[], [1], [2]])
        sync_mount_table("//tmp/q")
        self._sync_replication_era(card_id, replicas[:1])

        for i in (0, 1, 2):
            insert_rows("//tmp/q", [{"key": i, "value": str(i)}])

        sync_unmount_table("//tmp/q")
        set("//tmp/q/@enable_replication_progress_advance_to_barrier", False)
        sync_mount_table("//tmp/q")

        timestamp = generate_timestamp()
        replication_progress = {
            "segments": [
                {"lower_key": [0], "timestamp": timestamp},
                {"lower_key": [1], "timestamp": 0},
                {"lower_key": [2], "timestamp": timestamp},
            ],
            "upper_key": [yson.to_yson_type(None, attributes={"type": b"max"})]
        }

        def _pull_rows(response_parameters=None):
            return pull_rows(
                "//tmp/q",
                replication_progress=replication_progress,
                upstream_replica_id=replica_ids[0],
                response_parameters=response_parameters)
        wait(lambda: len(_pull_rows()) == 1)

        response_parameters={}
        rows = _pull_rows(response_parameters)
        print_debug(response_parameters)

        row = rows[0]
        assert row["key"] == 1
        assert str(row["value"][0]) == "1"
        assert len(row.attributes["write_timestamps"]) == 1
        assert row.attributes["write_timestamps"][0] > 0

        progress = response_parameters["replication_progress"]
        assert progress["segments"][0] == replication_progress["segments"][0]
        assert progress["segments"][2] == replication_progress["segments"][2]
        assert str(progress["upper_key"]) == str(replication_progress["upper_key"])
        assert progress["segments"][1]["timestamp"] >= row.attributes["write_timestamps"][0]

    @authors("savrus")
    def test_end_replication_row_index(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": False, "replica_path": "//tmp/t"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        def _pull_rows(upper_timestamp=0, response_parameters=None):
            return pull_rows(
                "//tmp/q",
                upstream_replica_id=replica_ids[0],
                replication_progress={
                    "segments": [{"lower_key": [], "timestamp": 0}],
                    "upper_key": [yson.to_yson_type(None, attributes={"type": "max"})]
                },
                upper_timestamp=upper_timestamp,
                response_parameters=response_parameters)

        timestamp = generate_timestamp()
        insert_rows("//tmp/q", [{"key": 0, "value": "0"}])
        wait(lambda: len(_pull_rows()) == 1)

        def _check(timestamp, expected_row_count, expected_end_index, expected_progress):
            response_parameters={}
            rows = _pull_rows(timestamp, response_parameters)
            print_debug(response_parameters)
            end_indexes = list(response_parameters["end_replication_row_indexes"].values())

            assert len(rows) == expected_row_count
            assert len(end_indexes) == 1
            assert end_indexes[0] == expected_end_index

            segments = response_parameters["replication_progress"]["segments"]
            assert len(segments) == 1
            if expected_progress:
                assert segments[0]["timestamp"] == expected_progress

        _check(0, 1, 1, None)
        _check(timestamp, 0, 0, timestamp)

    @authors("savrus")
    def test_delete_rows_replication(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)

        delete_rows("//tmp/t", [{"key": 0}])
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == [])

        rows = lookup_rows("//tmp/t", [{"key": 0}], versioned=True)
        row = rows[0]
        assert len(row.attributes["write_timestamps"]) == 1
        assert len(row.attributes["delete_timestamps"]) == 1

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_replica_enable(self, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": mode, "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        orchid = self._get_table_orchids("//tmp/t")[0]
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["mode"] == mode
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["state"] == "disabled"
        assert orchid["write_mode"] == "pull"

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)

        assert lookup_rows("//tmp/t", [{"key": 0}]) == []
        alter_table_replica(replica_ids[0], enabled=True)

        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)
        orchid = self._get_table_orchids("//tmp/t")[0]
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["mode"] == mode
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["state"] == "enabled"
        assert orchid["write_mode"] == "pull" if mode == "async" else "direct"

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_replica_disable(self, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": mode, "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        orchid = self._get_table_orchids("//tmp/t")[0]
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["mode"] == mode
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["state"] == "enabled"
        assert orchid["write_mode"] == "pull" if mode == "async" else "direct"

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)

        self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=False)

        values = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values)

        orchid = self._get_table_orchids("//tmp/t")[0]
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["mode"] == mode
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["state"] == "disabled"
        assert orchid["write_mode"] == "pull"
        assert lookup_rows("//tmp/t", [{"key": 1}]) == []

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_replica_mode_switch(self, mode):
        if mode == "sync":
            old_mode, new_mode, old_write_mode, new_wirte_mode = "sync", "async", "direct", "pull"
        else:
            old_mode, new_mode, old_write_mode, new_wirte_mode = "async", "sync", "pull", "direct"

        def _check_lookup(keys, values, mode):
            if mode == "sync":
                assert lookup_rows("//tmp/t", keys) == values
            else:
                assert lookup_rows("//tmp/t", keys) == []

        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": old_mode, "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        orchid = self._get_table_orchids("//tmp/t")[0]
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["mode"] == old_mode
        assert orchid["replication_card"]["replicas"][replica_ids[0]]["state"] == "enabled"
        assert orchid["write_mode"] == old_write_mode

        values = [{"key": i, "value": str(i)} for i in range(2)]
        insert_rows("//tmp/t", values[:1])

        _check_lookup([{"key": 0}], values[:1], old_mode)
        self._sync_alter_replica(card_id, replicas, replica_ids, 0, mode=new_mode)

        self._insistent_insert_rows("//tmp/t", values[1:])
        _check_lookup([{"key": 1}], values[1:], new_mode)

        orchid = self._get_table_orchids("//tmp/t")[0]
        replica_id = replica_ids[0]
        assert orchid["replication_card"]["replicas"][replica_id]["mode"] == new_mode
        assert orchid["replication_card"]["replicas"][replica_id]["state"] == "enabled"
        assert orchid["write_mode"] == new_wirte_mode

    @authors("savrus")
    def test_queue_replica_mode_stuck_in_cataclysm(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        alter_table_replica(replica_ids[1], mode="async")
        wait(lambda: get("#{0}/@coordinator_cell_ids".format(card_id)) == [])
        assert get("#{0}/@mode".format(replica_ids[1])) == "sync_to_async"

        with self.CellsDisabled(clusters=self.get_cluster_names(), chaos_bundles=["c"]):
            pass

        assert get("#{0}/@mode".format(replica_ids[1])) == "sync_to_async"
        assert get("#{0}/@coordinator_cell_ids".format(card_id)) == []

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, mode="sync")

        values = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values)
        assert lookup_rows("//tmp/t", [{"key": 1}]) == values

    @authors("savrus")
    def test_queue_replica_state_stuck_in_cataclysm(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        alter_table_replica(replica_ids[1], enabled=False)
        wait(lambda: get("#{0}/@coordinator_cell_ids".format(card_id)) == [])
        assert get("#{0}/@state".format(replica_ids[1])) == "disabling"

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=True)

        values = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values)
        assert lookup_rows("//tmp/t", [{"key": 1}]) == values

    @authors("savrus")
    def test_replication_progress(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)
        tablet_id = get("//tmp/t/@tablets/0/tablet_id")

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)

        self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=False)

        orchid = get("#{0}/orchid".format(tablet_id))
        progress = orchid["replication_progress"]

        sync_unmount_table("//tmp/t")
        assert get("#{0}/@replication_progress".format(tablet_id)) == progress

        sync_mount_table("//tmp/t")
        orchid = get("#{0}/orchid".format(tablet_id))
        assert orchid["replication_progress"] == progress

        cell_id = get("#{0}/@cell_id".format(tablet_id))
        build_snapshot(cell_id=cell_id)
        peer = get("//sys/tablet_cells/{}/@peers/0/address".format(cell_id))
        set("//sys/cluster_nodes/{}/@banned".format(peer), True)
        wait_for_cells([cell_id], decommissioned_addresses=[peer])

        orchid = get("#{0}/orchid".format(tablet_id))
        assert orchid["replication_progress"] == progress

    @authors("savrus")
    def test_async_queue_replica(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "primary", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        def _pull_rows(replica_index, versioned=False):
            rows = pull_rows(
                replicas[replica_index]["replica_path"],
                replication_progress={"segments": [{"lower_key": [], "timestamp": 0}], "upper_key": ["#<Max>"]},
                upstream_replica_id=replica_ids[replica_index],
                driver=get_driver(cluster=replicas[replica_index]["cluster_name"]))
            if versioned:
                return rows
            return [{"key": row["key"], "value": str(row["value"][0])} for row in rows]

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/q", values)
        wait(lambda: _pull_rows(2) == values)
        wait(lambda: _pull_rows(1) == values)

        async_rows = _pull_rows(1, True)
        sync_rows = _pull_rows(2, True)
        assert async_rows == sync_rows

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_queue_replica_enable_disable(self, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "primary", "content_type": "queue", "mode": mode, "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        def _pull_rows(replica_index):
            rows = pull_rows(
                replicas[replica_index]["replica_path"],
                replication_progress={"segments": [{"lower_key": [], "timestamp": 0}], "upper_key": ["#<Max>"]},
                upstream_replica_id=replica_ids[replica_index],
                driver=get_driver(cluster=replicas[replica_index]["cluster_name"]))
            return [{"key": row["key"], "value": str(row["value"][0])} for row in rows]

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)
        wait(lambda: _pull_rows(2) == values0)
        wait(lambda: _pull_rows(1) == values0)

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=False)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)

        wait(lambda: _pull_rows(2) == values0 + values1)
        assert _pull_rows(1) == values0

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=True)

        wait(lambda: _pull_rows(1) == values0 + values1)

        values2 = [{"key": 2, "value": "2"}]
        insert_rows("//tmp/t", values2)
        values = values0 + values1 + values2
        wait(lambda: _pull_rows(2) == values)
        wait(lambda: _pull_rows(1) == values)

    @authors("savrus")
    @pytest.mark.parametrize("content", ["data", "queue", "both"])
    def test_resharded_replication(self, content):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False)

        if content in ["data", "both"]:
            reshard_table("//tmp/t", [[], [1]])
        if content in ["queue", "both"]:
            reshard_table("//tmp/q", [[], [1]], driver=get_driver(cluster="remote_0"))

        for replica in replicas:
            sync_mount_table(replica["replica_path"], driver=get_driver(cluster=replica["cluster_name"]))
        self._sync_replication_era(card_id, replicas)

        rows = [{"key": i, "value": str(i)} for i in range(2)]
        keys = [{"key": row["key"]} for row in rows]
        insert_rows("//tmp/t", rows)
        wait(lambda: lookup_rows("//tmp/t", keys) == rows)

        rows = [{"key": i, "value": str(i+2)} for i in range(2)]
        for i in reversed(list(range(2))):
            insert_rows("//tmp/t", [rows[i]])
        wait(lambda: lookup_rows("//tmp/t", keys) == rows)

    @authors("savrus")
    def test_resharded_queue_pull(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q0"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/q1"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False)

        reshard_table("//tmp/q0", [[], [1]], driver=get_driver(cluster="remote_0"))

        for replica in replicas:
            sync_mount_table(replica["replica_path"], driver=get_driver(cluster=replica["cluster_name"]))
        self._sync_replication_era(card_id, replicas)

        def _pull_rows(replica_index, timestamp):
            rows = pull_rows(
                replicas[replica_index]["replica_path"],
                replication_progress={"segments": [{"lower_key": [], "timestamp": timestamp}], "upper_key": ["#<Max>"]},
                upstream_replica_id=replica_ids[replica_index],
                driver=get_driver(cluster=replicas[replica_index]["cluster_name"]))
            return [{"key": row["key"], "value": str(row["value"][0])} for row in rows]

        values0 = [{"key": i, "value": str(i)} for i in range(2)]
        insert_rows("//tmp/t", values0)
        wait(lambda: _pull_rows(1, 0) == values0)
        wait(lambda: _pull_rows(2, 0) == values0)

        timestamp = generate_timestamp()
        values1 = [{"key": i, "value": str(i+10)} for i in range(2)]
        insert_rows("//tmp/t", values1)
        wait(lambda: _pull_rows(1, timestamp) == values1)
        wait(lambda: _pull_rows(2, timestamp) == values1)

    @authors("babenko")
    def test_chaos_replicated_table_requires_valid_card_id(self):
        with pytest.raises(YtError):
            create("chaos_replicated_table", "//tmp/crt")
            create("chaos_replicated_table", "//tmp/crt", attributes={"replication_card_id": "1-2-3-4"})

    @authors("savrus")
    def test_chaos_replicated_table_requires_bundle_use(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        card_id = create_replication_card(chaos_cell_id=cell_id)
        create_user("u")

        def _create(path, card_id=None):
            attributes = {"chaos_cell_bundle": "c"}
            if card_id:
                attributes["replication_card_id"] = card_id
            create("chaos_replicated_table", path, authenticated_user="u", attributes=attributes)

        with pytest.raises(YtError):
            _create("//tmp/crt", card_id)

        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)
        with pytest.raises(YtError):
            _create("//tmp/crt")

        set("//sys/chaos_cell_bundles/c/@acl/end", make_ace("allow", "u", "use"))
        _create("//tmp/crt", card_id)
        _create("//tmp/crt2")

    @authors("babenko")
    def test_chaos_replicated_table_with_explicit_card_id(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        card_id = create_replication_card(chaos_cell_id=cell_id)

        create("chaos_replicated_table", "//tmp/crt", attributes={
            "chaos_cell_bundle": "c",
            "replication_card_id": card_id
        })
        assert get("//tmp/crt/@type") == "chaos_replicated_table"
        assert get("//tmp/crt/@replication_card_id") == card_id
        assert get("//tmp/crt/@owns_replication_card")

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, replication_card_id=card_id)
        assert len(replica_ids) == 2

        wait(lambda: get("#{0}/@era".format(card_id)) == 1)

        card = get("#{0}/@".format(card_id))
        assert get("//tmp/crt/@era") == card["era"]
        assert get("//tmp/crt/@coordinator_cell_ids") == card["coordinator_cell_ids"]

        crt_replicas = get("//tmp/crt/@replicas")
        assert len(crt_replicas) == 2

        def _assert_replicas_equal(lhs, rhs):
            return all(lhs[key] == rhs[key] for key in ("cluster_name", "replica_path", "mode", "state", "content_type"))

        for replica_id in replica_ids:
            replica = card["replicas"][replica_id]
            del replica["history"]
            del replica["replication_progress"]
            _assert_replicas_equal(replica, crt_replicas[replica_id])

    @authors("babenko")
    def test_chaos_replicated_table_replication_card_ownership(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        card_id = create_replication_card(chaos_cell_id=cell_id)

        create("chaos_replicated_table", "//tmp/crt", attributes={
            "replication_card_id": card_id,
            "chaos_cell_bundle": "c"
        })
        create("chaos_replicated_table", "//tmp/crt_view", attributes={
            "replication_card_id": card_id,
            "owns_replication_card": False,
            "chaos_cell_bundle": "c"
        })

        assert get("//tmp/crt/@owns_replication_card")
        assert not get("//tmp/crt_view/@owns_replication_card")

        assert exists("#{0}".format(card_id))
        remove("//tmp/crt_view")
        time.sleep(1)
        assert exists("#{0}".format(card_id))

        tx = start_transaction()
        set("//tmp/crt/@attr", "value", tx=tx)
        commit_transaction(tx)
        time.sleep(1)
        assert exists("#{0}".format(card_id))

        remove("//tmp/crt")
        wait(lambda: not exists("#{0}".format(card_id)))

    @authors("savrus")
    @pytest.mark.parametrize("all_keys", [True, False])
    def test_get_in_sync_replicas(self, all_keys):
        def _get_in_sync_replicas():
            if all_keys:
                return get_in_sync_replicas("//tmp/t", [], all_keys=True, timestamp=MaxTimestamp)
            else:
                return get_in_sync_replicas("//tmp/t", [{"key": 0}], timestamp=MaxTimestamp)

        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        sync_replicas = _get_in_sync_replicas()
        assert len(sync_replicas) == 0

        self._sync_alter_replica(card_id, replicas, replica_ids, 0, mode="sync")

        sync_replicas = _get_in_sync_replicas()
        assert len(sync_replicas) == 1

        self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=False)

        rows = [{"key": 0, "value": "0"}]
        keys = [{"key": 0}]
        insert_rows("//tmp/t", rows)

        sync_unmount_table("//tmp/t")
        alter_table_replica(replica_ids[0], enabled=True)

        sync_replicas = _get_in_sync_replicas()
        assert len(sync_replicas) == 0

        sync_mount_table("//tmp/t")
        wait(lambda: lookup_rows("//tmp/t", keys) == rows)

        def _check_progress():
            card = get("#{0}/@".format(card_id))
            replica = card["replicas"][replica_ids[0]]
            if replica["state"] != "enabled":
                return False
            if replica["replication_progress"]["segments"][0]["timestamp"] < replica["history"][-1]["timestamp"]:
                return False
            return True

        wait(_check_progress)

        sync_replicas = _get_in_sync_replicas()
        assert len(sync_replicas) == 1

    @authors("savrus")
    def test_async_get_in_sync_replicas(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)
        sync_unmount_table("//tmp/t")
        reshard_table("//tmp/t", [[], [1]])

        rows = [{"key": i, "value": str(i)} for i in range(2)]
        keys = [{"key": row["key"]} for row in rows]
        insert_rows("//tmp/t", rows)
        timestamp0 = generate_timestamp()

        def _check(keys, expected):
            sync_replicas = get_in_sync_replicas("//tmp/t", keys, timestamp=timestamp0)
            assert len(sync_replicas) == expected

        _check(keys[:1], 0)
        _check(keys[1:], 0)
        _check(keys, 0)

        def _check_progress(segment_index=0):
            card = get("#{0}/@".format(card_id))
            replica = card["replicas"][replica_ids[0]]
            # Segment index may not be strict since segments with the same timestamp are glued into one.
            segment_index = min(segment_index, len(replica["replication_progress"]["segments"]) - 1)

            if replica["state"] != "enabled":
                return False
            if replica["replication_progress"]["segments"][segment_index]["timestamp"] < timestamp0:
                return False
            return True

        sync_mount_table("//tmp/t", first_tablet_index=0, last_tablet_index=0)
        wait(lambda: lookup_rows("//tmp/t", keys[:1]) == rows[:1])
        wait(lambda: _check_progress(0))

        _check(keys[:1], 1)
        _check(keys[1:], 0)
        _check(keys, 0)

        sync_mount_table("//tmp/t", first_tablet_index=1, last_tablet_index=1)
        wait(lambda: lookup_rows("//tmp/t", keys[1:]) == rows[1:])
        wait(lambda: _check_progress(1))

        _check(keys[:1], 1)
        _check(keys[1:], 1)
        _check(keys, 1)

    @authors("babenko")
    def test_replication_card_attributes(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        TABLE_ID = "1-2-4b6-3"
        TABLE_PATH = "//path/to/table"
        TABLE_CLUSTER_NAME = "remote0"

        card_id = create_replication_card(chaos_cell_id=cell_id, attributes={
            "table_id": TABLE_ID,
            "table_path": TABLE_PATH,
            "table_cluster_name": TABLE_CLUSTER_NAME
        })

        attributes = get("#{0}/@".format(card_id))
        assert attributes["table_id"] == TABLE_ID
        assert attributes["table_path"] == TABLE_PATH
        assert attributes["table_cluster_name"] == TABLE_CLUSTER_NAME

    @authors("savrus")
    def test_metadata_chaos_cell(self):
        self._create_chaos_cell_bundle(name="c1")
        self._create_chaos_cell_bundle(name="c2")

        assert not exists("//sys/chaos_cell_bundles/c1/@metadata_cell_id")

        with pytest.raises(YtError, match="No cell with id .* is known"):
            set("//sys/chaos_cell_bundles/c1/@metadata_cell_id", "1-2-3-4")

        cell_id1 = self._sync_create_chaos_cell(name="c1")
        set("//sys/chaos_cell_bundles/c1/@metadata_cell_id", cell_id1)
        assert get("#{0}/@ref_counter".format(cell_id1)) == 1

        cell_id2 = self._sync_create_chaos_cell(name="c1")
        set("//sys/chaos_cell_bundles/c1/@metadata_cell_id", cell_id2)
        assert get("#{0}/@ref_counter".format(cell_id2)) == 1

        remove("//sys/chaos_cell_bundles/c1/@metadata_cell_id")
        assert not exists("//sys/chaos_cell_bundles/c1/@metadata_cell_id")

        cell_id3 = self._sync_create_chaos_cell(name="c2")
        with pytest.raises(YtError, match="Cell .* belongs to a different bundle .*"):
            set("//sys/chaos_cell_bundles/c1/@metadata_cell_id", cell_id3)

    @authors("babenko")
    def test_chaos_replicated_table_with_implicit_card_id(self):
        with pytest.raises(YtError, match=".* is neither speficied nor inherited.*"):
            create("chaos_replicated_table", "//tmp/crt")

        cell_id = self._sync_create_chaos_bundle_and_cell(name="chaos_bundle")

        with pytest.raises(YtError, match="Chaos cell bundle .* has no associated metadata chaos cell"):
            create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "chaos_bundle"})

        set("//sys/chaos_cell_bundles/chaos_bundle/@metadata_cell_id", cell_id)

        table_id = create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "chaos_bundle"})

        assert get("//tmp/crt/@type") == "chaos_replicated_table"
        assert get("//tmp/crt/@owns_replication_card")

        card_id = get("//tmp/crt/@replication_card_id")

        assert exists("#{0}".format(card_id))
        card = get("#{0}/@".format(card_id))
        assert card["table_id"] == table_id
        assert card["table_path"] == "//tmp/crt"
        assert card["table_cluster_name"] == "primary"

        remove("//tmp/crt")
        wait(lambda: not exists("#{0}".format(card_id)))

    @authors("babenko")
    def test_create_chaos_table_replica_for_chaos_replicated_table(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        card_id = create_replication_card(chaos_cell_id=cell_id)

        create("chaos_replicated_table", "//tmp/crt", attributes={
            "chaos_cell_bundle": "c",
            "replication_card_id": card_id
        })

        replica = {"cluster_name": "remote_0", "content_type": "data", "mode": "sync", "replica_path": "//tmp/r0"}
        replica_id = self._create_chaos_table_replica(replica, table_path="//tmp/crt")

        attributes = get("#{0}/@".format(replica_id))
        assert attributes["type"] == "chaos_table_replica"
        assert attributes["id"] == replica_id
        assert attributes["replication_card_id"] == card_id
        assert attributes["replica_path"] == "//tmp/r0"
        assert attributes["cluster_name"] == "remote_0"

    @authors("savrus")
    def test_chaos_table_alter(self):
        cell_id = self._sync_create_chaos_bundle_and_cell(name="chaos_bundle")
        set("//sys/chaos_cell_bundles/chaos_bundle/@metadata_cell_id", cell_id)

        schema = yson.YsonList([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ])
        create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "chaos_bundle", "schema": schema})

        def _validate_schema(schema):
            actual_schema = get("//tmp/crt/@schema")
            assert actual_schema.attributes["unique_keys"]
            for column, actual_column in zip_longest(schema, actual_schema):
                for name, value in column.items():
                    assert actual_column[name] == value
        _validate_schema(schema)

        schema = yson.YsonList([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
            {"name": "new_value", "type": "string"},
        ])
        alter_table("//tmp/crt", schema=schema)
        assert get("//tmp/crt/@dynamic")
        _validate_schema(schema)

    @authors("savrus")
    def test_invalid_chaos_table_data_access(self):
        cell_id = self._sync_create_chaos_bundle_and_cell(name="chaos_bundle")
        set("//sys/chaos_cell_bundles/chaos_bundle/@metadata_cell_id", cell_id)
        create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "chaos_bundle"})

        with pytest.raises(YtError, match="Table schema is not specified"):
            insert_rows("//tmp/crt", [{"key": 0, "value": "0"}])
        with pytest.raises(YtError, match="Table schema is not specified"):
            lookup_rows("//tmp/crt", [{"key": 0}])

    @authors("savrus")
    def test_chaos_table_data_access(self):
        cell_id = self._sync_create_chaos_bundle_and_cell(name="chaos_bundle")
        set("//sys/chaos_cell_bundles/chaos_bundle/@metadata_cell_id", cell_id)
        create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "chaos_bundle"})

        replicas = [
            {"cluster_name": "remote_0", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"}
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")
        self._create_replica_tables(replicas, replica_ids)

        card_id = get("//tmp/crt/@replication_card_id")
        self._sync_replication_era(card_id, replicas)

        schema = yson.YsonList([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ])
        alter_table("//tmp/crt", schema=schema)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/crt", values)
        wait(lambda: lookup_rows("//tmp/crt", [{"key": 0}]) == values)
        assert select_rows("* from [//tmp/crt]") == values

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_chaos_table_and_dynamic_table(self, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell(name="chaos_bundle")
        set("//sys/chaos_cell_bundles/chaos_bundle/@metadata_cell_id", cell_id)
        schema = yson.YsonList([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ])
        create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "chaos_bundle", "schema": schema})

        replicas = [
            {"cluster_name": "remote_0", "content_type": "data", "mode": mode, "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"}
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")
        self._create_replica_tables(replicas, replica_ids)
        card_id = get("//tmp/crt/@replication_card_id")
        self._sync_replication_era(card_id, replicas)

        sync_create_cells(1)
        self._create_sorted_table("//tmp/d")
        sync_mount_table("//tmp/d")

        values = [{"key": 0, "value": "0"}]
        tx = start_transaction(type="tablet")
        insert_rows("//tmp/crt", values, tx=tx)
        insert_rows("//tmp/d",  [{"key": 0, "value": "1"}], tx=tx)
        commit_transaction(tx)

        row = lookup_rows("//tmp/d", [{"key": 0}], versioned=True)
        assert str(row[0]["value"][0]) == "1"
        ts = row[0].attributes["write_timestamps"][0]

        if mode == "async":
            def _insistent_lookup_rows():
                try:
                    return lookup_rows("//tmp/crt", [{"key": 0}], timestamp=ts)
                except YtError as err:
                    if err.contains_text("No in-sync replicas found for table //tmp/crt"):
                        return []
                    raise err
            wait(lambda: _insistent_lookup_rows() == values)
            row = lookup_rows("//tmp/crt", [{"key": 0}], timestamp=ts, versioned=True)
        else:
            row = lookup_rows("//tmp/crt", [{"key": 0}], versioned=True)

        assert str(row[0]["value"][0]) == "0"
        assert row[0].attributes["write_timestamps"][0] == ts

    @authors("savrus")
    @pytest.mark.parametrize("disable_data", [True, False])
    @pytest.mark.parametrize("wait_alter", [True, False])
    def test_trim_replica_history_items(self, disable_data, wait_alter):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        def _alter_table_replica(replica_id, mode):
            alter_table_replica(replica_id, mode=mode)
            if wait_alter:
                wait(lambda: get("#{0}/@mode".format(replica_id)) == mode)

        _alter_table_replica(replica_ids[2], "sync")
        _alter_table_replica(replica_ids[1], "async")
        if disable_data:
            self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=False)
        _alter_table_replica(replica_ids[1], "sync")
        _alter_table_replica(replica_ids[2], "async")
        _alter_table_replica(replica_ids[2], "sync")
        _alter_table_replica(replica_ids[1], "async")
        if disable_data:
            self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=True)

        wait(lambda: all(replica["mode"] in ["sync", "async"] for replica in get("#{0}/@replicas".format(card_id)).values()))
        self._sync_replication_era(card_id)

        def _check():
            card = get("#{0}/@".format(card_id))
            if any(len(replica["history"]) > 1 for replica in card["replicas"].values()):
                return False
            return True
        wait(_check)

        rows = [{"key": 1, "value": "1"}]
        keys = [{"key": 1}]
        self._insistent_insert_rows("//tmp/t", rows)
        wait(lambda: lookup_rows("//tmp/t", keys) == rows)

    @authors("savrus")
    def test_queue_trimming(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        def _check(value, row_count):
            rows = [{"key": 1, "value": value}]
            keys = [{"key": 1}]
            insert_rows("//tmp/t", rows)
            wait(lambda: lookup_rows("//tmp/t", keys) == rows)
            wait(lambda: get("//tmp/q/@tablets/0/trimmed_row_count", driver=remote_driver0) == row_count)
            sync_flush_table("//tmp/q", driver=remote_driver0)
            wait(lambda: len(get("//tmp/q/@chunk_ids", driver=remote_driver0)) == 0)

        _check("1", 1)
        _check("2", 2)

    @authors("savrus")
    def test_initially_disabled_replica(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "async", "enabled": False, "replica_path": "//tmp/r1"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)
        assert get("#{0}/@".format(card_id))["replicas"][replica_ids[2]]["state"] == "disabled"

    @authors("savrus")
    def test_new_data_replica(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_0", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas[:2])

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)

        replica_ids.append(self._create_chaos_table_replica(replicas[2], replication_card_id=card_id, catchup=False))
        self._create_replica_tables(replicas[2:], replica_ids[2:])
        self._sync_replication_era(card_id, replicas)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)

        wait(lambda: select_rows("* from [//tmp/t]") == values0 + values1)
        assert(select_rows("* from [//tmp/r]", driver=get_driver(cluster="remote_0")) == values1)

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_new_data_replica_catchup(self, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_0", "content_type": "data", "mode": mode, "enabled": True, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas[:2])

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)

        replica_ids.append(self._create_chaos_table_replica(replicas[2], replication_card_id=card_id, catchup=True))
        self._create_replica_tables(replicas[2:], replica_ids[2:])
        if mode == "sync":
            self._sync_replication_era(card_id, replicas)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)
        wait(lambda: select_rows("* from [//tmp/r]", driver=get_driver(cluster="remote_0")) == values0 + values1)

    @authors("savrus")
    @pytest.mark.parametrize("enabled", [True, False])
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_new_data_replica_with_progress_and_wo_catchup(self, mode, enabled):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_0", "content_type": "data", "mode": mode, "enabled": enabled, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas[:2])
        _, remote_driver0, remote_driver1 = self._get_drivers()

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)

        replica_ids.append(self._create_chaos_table_replica(replicas[2], replication_card_id=card_id, catchup=False))
        self._create_replica_tables(replicas[2:], replica_ids[2:], mount_tables=False)

        sync_unmount_table("//tmp/t")
        replication_progress = get("//tmp/t/@replication_progress")
        sync_mount_table("//tmp/t")
        alter_table("//tmp/r", replication_progress=replication_progress, driver=remote_driver0)
        sync_mount_table("//tmp/r", driver=remote_driver0)

        if mode == "sync":
            self._sync_replication_era(card_id, replicas)

        if not enabled:
            self._sync_alter_replica(card_id, replicas, replica_ids, 2, enabled=True)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)

        if mode == "sync":
            assert select_rows("* from [//tmp/r]", driver=get_driver(cluster="remote_0")) == values1
        else:
            wait(lambda: select_rows("* from [//tmp/r]", driver=get_driver(cluster="remote_0")) == values1)

    @authors("savrus")
    @pytest.mark.parametrize("catchup", [True, False])
    def test_new_queue_replica(self, catchup):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q0"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q1"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas[:2])

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)

        replica_ids.append(self._create_chaos_table_replica(replicas[2], replication_card_id=card_id, catchup=catchup))
        self._create_replica_tables(replicas[2:], replica_ids[2:])
        self._sync_replication_era(card_id, replicas)

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=False)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)

        self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=True)

        expected = values0 + values1 if catchup else []
        wait(lambda: select_rows("* from [//tmp/t]") == expected)

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=True)
        wait(lambda: select_rows("* from [//tmp/t]") == values0 + values1)

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_new_replica_with_progress(self, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        drivers = self._get_drivers()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": mode, "enabled": True, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas[:2])

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)

        timestamp = generate_timestamp()
        replication_progress = {
            "segments": [{
                "lower_key": [],
                "timestamp": timestamp
            }],
            "upper_key": [yson.to_yson_type(None, attributes={"type": "max"})]
        }
        print_debug("Creating new replica with progress:", replication_progress)

        replica_ids.append(self._create_chaos_table_replica(replicas[2], replication_card_id=card_id, replication_progress=replication_progress))
        self._create_replica_tables(replicas[2:], replica_ids[2:])
        if mode == "sync":
            self._sync_replication_era(card_id, replicas)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)

        self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=True)
        wait(lambda: select_rows("* from [//tmp/t]") == values0 + values1)

        if mode == "sync":
            assert(select_rows("* from [//tmp/r]", driver=drivers[2]) == values1)
        else:
            wait(lambda: select_rows("* from [//tmp/r]", driver=drivers[2]) == values1)

    @authors("savrus")
    def test_coordinator_suspension(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        coordinator_cell_id = self._sync_create_chaos_cell()
        assert len(get("//sys/chaos_cells")) == 2

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas[:2])

        def _get_orchid(cell_id, path):
            address = get("#{0}/@peers/0/address".format(cell_id))
            return get("//sys/cluster_nodes/{0}/orchid/chaos_cells/{1}{2}".format(address, cell_id, path))

        def _get_shortcuts(cell_id):
            return _get_orchid(cell_id, "/coordinator_manager/shortcuts")

        assert list(_get_shortcuts(cell_id).keys()) == [card_id]
        assert list(_get_shortcuts(coordinator_cell_id).keys()) == [card_id]

        chaos_cell_ids = [cell_id, coordinator_cell_id]
        assert_items_equal(get("#{0}/@coordinator_cell_ids".format(card_id)), chaos_cell_ids)

        suspend_coordinator(coordinator_cell_id)
        assert _get_orchid(coordinator_cell_id, "/coordinator_manager/internal/suspended")
        wait(lambda: get("#{0}/@coordinator_cell_ids".format(card_id)) == [cell_id])

        resume_coordinator(coordinator_cell_id)
        assert not _get_orchid(coordinator_cell_id, "/coordinator_manager/internal/suspended")
        wait(lambda: sorted(get("#{0}/@coordinator_cell_ids".format(card_id))) == sorted(chaos_cell_ids))

    @authors("savrus")
    def test_nodes_restart(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 0}], driver=remote_driver1) == values0)

        with Restarter(self.Env, NODES_SERVICE):
            pass
        with Restarter(self.Env, CHAOS_NODES_SERVICE):
            pass

        wait_for_chaos_cell(cell_id, self.get_cluster_names())
        for driver in self._get_drivers():
            cell_ids = get("//sys/tablet_cell_bundles/default/@tablet_cell_ids", driver=driver)
            wait_for_cells(cell_ids, driver=driver)

        assert lookup_rows("//tmp/r1", [{"key": 0}], driver=remote_driver1) == values0
        self._sync_replication_era(card_id, replicas)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 1}], driver=remote_driver1) == values1)

    @authors("savrus")
    def test_replication_progress_attribute(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "sync", "enabled": False, "replica_path": "//tmp/r"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        reshard_table("//tmp/t", [[], [1]])
        reshard_table("//tmp/q", [[], [1]], driver=remote_driver0)
        reshard_table("//tmp/r", [[], [1]], driver=remote_driver1)

        def _sync_mount_tables(replicas):
            for replica in replicas:
                sync_mount_table(replica["replica_path"], driver=get_driver(cluster=replica["cluster_name"]))
            self._sync_replication_era(card_id, replicas)
        _sync_mount_tables(replicas[:2])

        def _insert_and_check(key, value):
            rows = [{"key": key, "value": value}]
            insert_rows("//tmp/t", rows)
            wait(lambda: lookup_rows("//tmp/t", [{"key": key}]) == rows)

        _insert_and_check(0, "0")
        sync_unmount_table("//tmp/q", first_tablet_index=0, last_tablet_index=0, driver=remote_driver0)
        _insert_and_check(1, "1")

        sync_unmount_table("//tmp/t")
        progress = get("//tmp/t/@replication_progress")
        assert len(progress["segments"]) > 1
        alter_table("//tmp/r", replication_progress=progress, driver=remote_driver1)
        reshard_table("//tmp/t", [[], [2]])

        _sync_mount_tables(replicas)
        self._sync_alter_replica(card_id, replicas, replica_ids, 2, enabled=True)

        _insert_and_check(2, "2")
        assert lookup_rows("//tmp/r", [{"key": 2}], driver=remote_driver1) == [{"key": 2, "value": "2"}]
        assert lookup_rows("//tmp/r", [{"key": i} for i in range(2)], driver=remote_driver1) == []
        _insert_and_check(1, "3")
        assert lookup_rows("//tmp/r", [{"key": 1}], driver=remote_driver1) == [{"key": 1, "value": "3"}]

    @authors("savrus")
    def test_copy_chaos_tables(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/q0"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q1"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/r"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values0)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values0)

        wait(lambda: get("//tmp/q0/@tablets/0/trimmed_row_count", driver=remote_driver0) == 1)
        wait(lambda: get("//tmp/q1/@tablets/0/trimmed_row_count", driver=remote_driver0) == 1)

        sync_unmount_table("//tmp/t")
        sync_unmount_table("//tmp/q0", driver=remote_driver0)

        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values1)

        sync_unmount_table("//tmp/q1", driver=remote_driver0)
        sync_unmount_table("//tmp/r", driver=remote_driver1)

        copy_replicas = []
        for replica in replicas:
            driver = get_driver(cluster=replica["cluster_name"])
            copy_replica = deepcopy(replica)
            copy_replica["replica_path"] = replica["replica_path"] + "-copy"
            replication_progress = get("{0}/@replication_progress".format(replica["replica_path"]), driver=driver)
            copy_replica["replication_progress"] = replication_progress
            copy_replicas.append(copy_replica)
            copy(replica["replica_path"], copy_replica["replica_path"], driver=driver)

        copy_card_id, copy_replica_ids = self._create_chaos_tables(
            cell_id,
            copy_replicas,
            create_replica_tables=False,
            create_tablet_cells=False,
            mount_tables=False,
            sync_replication_era=False)
        self._prepare_replica_tables(copy_replicas, copy_replica_ids)
        self._sync_replication_era(copy_card_id, copy_replicas)

        assert lookup_rows("//tmp/t-copy", [{"key": 0}]) == values0
        wait(lambda: lookup_rows("//tmp/t-copy", [{"key": 1}]) == values1)

        values2 = [{"key": 2, "value": "2"}]
        insert_rows("//tmp/t-copy", values2)
        assert lookup_rows("//tmp/r-copy", [{"key": 2}], driver=remote_driver1) == values2
        wait(lambda: lookup_rows("//tmp/t-copy", [{"key": 2}]) == values2)

    @authors("savrus")
    def test_lagging_queue(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q0"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q1"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        reshard_table("//tmp/q0", [[], [1]], driver=remote_driver0)
        reshard_table("//tmp/q1", [[], [1]], driver=remote_driver1)

        for replica in replicas:
            sync_mount_table(replica["replica_path"], driver=get_driver(cluster=replica["cluster_name"]))
        self._sync_replication_era(card_id, replicas)

        sync_unmount_table("//tmp/q1", driver=remote_driver1)

        timestamp = generate_timestamp()

        def _get_card_progress_timestamp():
            segments = get("#{0}/@replicas/{1}/replication_progress/segments".format(card_id, replica_ids[0]))
            return min(segment["timestamp"] for segment in segments)
        wait(lambda: _get_card_progress_timestamp() > timestamp)

        sync_unmount_table("//tmp/t")
        sync_mount_table("//tmp/q1", first_tablet_index=0, last_tablet_index=0, driver=remote_driver1)

        values0 = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/q0", values0, driver=remote_driver0)
        sync_unmount_table("//tmp/q0", driver=remote_driver0)

        set("//tmp/q1/@enable_replication_progress_advance_to_barrier", False, driver=remote_driver1)
        sync_mount_table("//tmp/q1", driver=remote_driver1)
        sync_mount_table("//tmp/t")
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values0)

        sync_mount_table("//tmp/q0", driver=remote_driver0)
        values1 = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/q0", values1, driver=remote_driver0)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 1}]) == values1)

    @authors("savrus")
    def test_replica_data_access(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/r1"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        values = [{"key": 0, "value": "0"}]
        keys = [{"key": 0}]
        insert_rows("//tmp/t", values)
        assert lookup_rows("//tmp/t", keys, replica_consistency="sync") == values
        assert select_rows("* from [//tmp/t]", replica_consistency="sync") == values
        assert lookup_rows("//tmp/t", keys) == []
        assert select_rows("* from [//tmp/t]") == []

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_dual_replica_placement(self, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        amode = {"sync": "async", "async": "sync"}[mode]
        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": mode, "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "primary", "content_type": "queue", "mode": mode, "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": amode, "enabled": True, "replica_path": "//tmp/r"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": amode, "enabled": True, "replica_path": "//tmp/s"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        values = [{"key": i, "value": str(i)} for i in range(10)]
        keys = [{"key": i} for i in range(10)]

        tx = start_transaction(type="tablet")
        for i in range(10):
            insert_rows("//tmp/t", values[i:i+1], tx=tx)
            time.sleep(1)
        commit_transaction(tx)

        assert lookup_rows("//tmp/t", keys, replica_consistency="sync") == values
        assert select_rows("* from [//tmp/t]", replica_consistency="sync") == values
        versined_rows = lookup_rows("//tmp/t", keys, replica_consistency="sync", versioned=True)
        rows = [{"key": row["key"], "value": str(row["value"][0])} for row in versined_rows]
        assert rows == values
        ts = versined_rows[0].attributes["write_timestamps"][0]
        assert all(row.attributes["write_timestamps"][0] == ts for row in versined_rows)

    @authors("shakurov")
    def test_chaos_cell_peer_snapshot_loss(self):
        cell_id = self._sync_create_chaos_bundle_and_cell(name="chaos_bundle")

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"}
        ]
        self._create_chaos_tables(cell_id, replicas)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)

        assert lookup_rows("//tmp/t", [{"key": 0}]) == values
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 0}], driver=remote_driver1) == values)

        build_snapshot(cell_id=cell_id)
        wait(lambda: len(ls("//sys/chaos_cells/{}/0/snapshots".format(cell_id))) != 0)
        wait(lambda: len(ls("//sys/chaos_cells/{}/1/snapshots".format(cell_id), driver=remote_driver0)) != 0)
        wait(lambda: len(ls("//sys/chaos_cells/{}/2/snapshots".format(cell_id), driver=remote_driver1)) != 0)

        assert get("//sys/chaos_cells/{}/@health".format(cell_id)) == "good"
        set("//sys/chaos_cell_bundles/chaos_bundle/@node_tag_filter", "empty_set_of_nodes", driver=remote_driver1)
        wait(lambda: get("//sys/chaos_cells/{}/@local_health".format(cell_id), driver=remote_driver1) != "good")

        remove("//sys/chaos_cells/{}/2/snapshots/*".format(cell_id), driver=remote_driver1)

        set("//sys/chaos_cell_bundles/chaos_bundle/@node_tag_filter", "", driver=remote_driver1)
        wait(lambda: get("//sys/chaos_cells/{}/@health".format(cell_id), driver=remote_driver1) == "good")
        assert len(ls("//sys/chaos_cells/{}/2/snapshots".format(cell_id), driver=remote_driver1)) != 0

    @authors("savrus")
    @pytest.mark.parametrize("tablet_count", [1, 2])
    def test_ordered_chaos_table_pull(self, tablet_count):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False, ordered=True)

        reshard_table("//tmp/t", tablet_count)

        for replica in replicas:
            sync_mount_table(replica["replica_path"], driver=get_driver(cluster=replica["cluster_name"]))
        self._sync_replication_era(card_id, replicas)

        def _pull_rows(replica_index, tablet_index, timestamp):
            rows = pull_rows(
                replicas[replica_index]["replica_path"],
                replication_progress={
                    "segments": [{"lower_key": [tablet_index], "timestamp": timestamp}],
                    "upper_key": [tablet_index + 1]
                },
                upstream_replica_id=replica_ids[replica_index],
                driver=get_driver(cluster=replicas[replica_index]["cluster_name"]))
            return [{"key": row["key"], "value": row["value"]} for row in rows]

        values = [{"$tablet_index": j, "key": i, "value": str(i + j)} for i in range(2) for j in range(tablet_count)]
        data_values = [[{"key": i, "value": str(i + j)} for i in range(2)] for j in range(tablet_count)]
        insert_rows("//tmp/t", values)

        for j in range(tablet_count):
            wait(lambda: select_rows("key, value from [//tmp/t] where [$tablet_index] = {0}".format(j)) == data_values[j])
            assert _pull_rows(0, j, 0) == data_values[j]

    @authors("savrus")
    @pytest.mark.parametrize("tablet_count", [1, 2])
    def test_ordered_chaos_table(self, tablet_count):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/r"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "async", "enabled": False, "replica_path": "//tmp/q"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, sync_replication_era=False, mount_tables=False, ordered=True)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        for replica in replicas:
            path = replica["replica_path"]
            driver = get_driver(cluster=replica["cluster_name"])
            reshard_table(path, tablet_count, driver=driver)
            sync_mount_table(path, driver=driver)
        self._sync_replication_era(card_id, replicas)

        values = [{"$tablet_index": j, "key": i, "value": str(i + j)} for i in range(2) for j in range(tablet_count)]
        data_values = [[{"key": i, "value": str(i + j)} for i in range(2)] for j in range(tablet_count)]
        insert_rows("//tmp/t", values)

        for j in range(tablet_count):
            wait(lambda: select_rows("key, value from [//tmp/t] where [$tablet_index] = {0}".format(j)) == data_values[j])
            wait(lambda: select_rows("key, value from [//tmp/r] where [$tablet_index] = {0}".format(j), driver=remote_driver0) == data_values[j])
            assert select_rows("key, value from [//tmp/q] where [$tablet_index] = {0}".format(j), replica_consistency="sync", driver=remote_driver1) == data_values[j]

        sync_unmount_table("//tmp/t")
        progress = get("//tmp/t/@replication_progress")
        assert len(progress["segments"]) <= tablet_count

    @authors("savrus")
    def test_invalid_ordered_chaos_table(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, create_replica_tables=False, sync_replication_era=False, mount_tables=False, ordered=True)

        self._create_ordered_table(
            "//tmp/t",
            upstream_replica_id=replica_ids[0],
            schema=[
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"},
            ]
        )
        self._prepare_replica_tables(replicas, replica_ids, mount_tables=False)
        with pytest.raises(YtError):
            sync_mount_table("//tmp/t")

        alter_table("//tmp/t", schema=[
            {"name": "key", "type": "int64"},
            {"name": "value", "type": "string"},
            {"name": "$timestamp", "type": "uint64"},
        ])
        sync_mount_table("//tmp/t")

        with raises_yt_error("Invalid input row for chaos ordered table //tmp/t: \"$tablet_index\" column is not provided"):
            insert_rows("//tmp/t", [{"key": 0, "value": str(0)}])

        sync_unmount_table("//tmp/t")
        set("//tmp/t/@commit_ordering", "weak")
        with pytest.raises(YtError):
            sync_mount_table("//tmp/t")

    @authors("savrus")
    def test_invalid_replication_log_table(self):
        with raises_yt_error("Table of type \"replication_log_table\" must be dynamic"):
            create("replication_log_table", "//tmp/r")

        with raises_yt_error("Could not create unsorted replication log table"):
            attributes = {
                "dynamic": True,
                "schema": [
                    {"name": "key", "type": "int64"},
                    {"name": "value", "type": "string"}],
            }
            create("replication_log_table", "//tmp/r", attributes=attributes)

    @authors("savrus")
    def test_ordered_chaos_table_trim(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "async", "enabled": False, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, ordered=True)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        data_values = [{"key": i, "value": str(i)} for i in range(3)]
        values = [{"$tablet_index": 0, "key": i, "value": str(i)} for i in range(1)]
        insert_rows("//tmp/t", values)

        def _try_trim_rows():
            with raises_yt_error("Could not trim tablet since some replicas may not be replicated up to this point"):
                trim_rows("//tmp/t", 0, 1)

        wait(lambda: select_rows("key, value from [//tmp/t]") == data_values[:1])
        _try_trim_rows()

        sync_flush_table("//tmp/t")
        _try_trim_rows()

        values = [{"$tablet_index": 0, "key": i, "value": str(i)} for i in range(1, 2)]
        insert_rows("//tmp/t", values)
        _try_trim_rows()

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=True)
        wait(lambda: select_rows("key, value from [//tmp/r]", driver=remote_driver0) == data_values[:2])

        def _insistent_trim_rows(table, driver=None):
            try:
                trim_rows(table, 0, 1, driver=driver)
                return True
            except YtError as err:
                print_debug("Table {0} trim failed: ".format(table), err)
                return False

        wait(lambda: _insistent_trim_rows("//tmp/t"))
        assert select_rows("key, value from [//tmp/t]") == data_values[1:2]

        sync_flush_table("//tmp/r", driver=remote_driver0)
        values = [{"$tablet_index": 0, "key": i, "value": str(i)} for i in range(2, 3)]
        insert_rows("//tmp/t", values)
        wait(lambda: _insistent_trim_rows("//tmp/r", driver=remote_driver0))
        assert select_rows("key, value from [//tmp/r]", driver=remote_driver0) == data_values[1:]

    @authors("savrus")
    def test_ordered_chaos_table_auto_trim(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "async", "enabled": False, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, ordered=True)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        set("//tmp/t/@min_data_ttl", 0)
        set("//tmp/t/@min_data_versions", 0)
        set("//tmp/t/@max_data_versions", 0)
        remount_table("//tmp/t")

        data_values = [{"key": i, "value": str(i)} for i in range(2)]
        values = [{"$tablet_index": 0, "key": i, "value": str(i)} for i in range(1)]
        insert_rows("//tmp/t", values)
        sync_flush_table("//tmp/t")

        assert select_rows("key, value from [//tmp/t]") == data_values[:1]

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=True)

        values = [{"$tablet_index": 0, "key": i, "value": str(i)} for i in range(1, 2)]
        insert_rows("//tmp/t", values)
        wait(lambda: select_rows("key, value from [//tmp/t]") == data_values[1:])

    @authors("savrus")
    def test_ordered_start_replication_row_index(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, ordered=True)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        data_values = [{"key": i, "value": str(i)} for i in range(3)]
        values = [{"$tablet_index": 0, "key": i, "value": str(i)} for i in range(3)]
        insert_rows("//tmp/t", values[:1])
        wait(lambda: select_rows("key, value from [//tmp/r]", driver=remote_driver0) == data_values[:1])

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, mode="sync")
        insert_rows("//tmp/t", values[1:2])
        wait(lambda: select_rows("key, value from [//tmp/r]", driver=remote_driver0) == data_values[:2])

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, mode="async")
        insert_rows("//tmp/t", values[2:3])
        wait(lambda: select_rows("key, value from [//tmp/r]", driver=remote_driver0) == data_values)

    @authors("savrus")
    @pytest.mark.parametrize("with_data", [True, False])
    @pytest.mark.parametrize("write_target", ["t", "q"])
    def test_locks_replication(self, with_data, write_target):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        schema = [
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "a", "type": "string", "lock": "a"},
            {"name": "b", "type": "string", "lock": "b"},
            {"name": "c", "type": "string", "lock": "c"},
        ]

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": False, "replica_path": "//tmp/r"},
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/s"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, schema=schema)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        tx1 = start_transaction(type="tablet")
        tx2 = start_transaction(type="tablet")

        path = "//tmp/" + write_target
        if with_data:
            insert_rows(path, [{"key": 1, "a": "1"}], update=True, tx=tx1)
        lock_rows(path, [{"key": 1}], locks=["a", "c"], tx=tx1, lock_type="shared_weak")
        insert_rows(path, [{"key": 1, "b": "2"}], update=True, tx=tx2)

        commit_transaction(tx1)
        commit_transaction(tx2)

        def _pull_rows():
            rows = pull_rows(
                replicas[3]["replica_path"],
                replication_progress={"segments": [{"lower_key": [], "timestamp": 0}], "upper_key": ["#<Max>"]},
                upstream_replica_id=replica_ids[3],
                driver=get_driver(cluster=replicas[3]["cluster_name"]))

            def _unversion(row):
                result = {"key": row["key"]}
                for value in "a", "b":
                    if value in row:
                        result[value] = str(row[value][0])
                return result
            return [_unversion(row) for row in rows]

        def _check():
            rows = _pull_rows()
            return len(rows) > 0 and "b" in rows[-1]
        wait(_check)

        self._sync_alter_replica(card_id, replicas, replica_ids, 3, mode="sync")
        self._sync_alter_replica(card_id, replicas, replica_ids, 2, enabled=False)
        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=True)

        expected = [{"key": 1, "a": "1", "b": "2", "c": None}] if with_data else [{"key": 1, "a": None, "b": "2", "c": None}]
        wait(lambda: lookup_rows("//tmp/r", [{"key": 1}]) == expected)

    @authors("savrus")
    def test_replicated_table_tracker(self):
        self._init_replicated_table_tracker()
        cell_id = self._sync_create_chaos_bundle_and_cell()
        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)

        replicated_table_options = {
            "enable_replicated_table_tracker": True,
            "min_sync_replica_count": 1,
            "tablet_cell_bundle_name_ttl": 1000,
            "tablet_cell_bundle_name_failure_interval": 100,
        }
        create("chaos_replicated_table", "//tmp/crt", attributes={
            "chaos_cell_bundle": "c",
            "replicated_table_options": replicated_table_options
        })
        card_id = get("//tmp/crt/@replication_card_id")
        options = get("//tmp/crt/@replicated_table_options")
        assert options["enable_replicated_table_tracker"]
        assert options["min_sync_replica_count"] == 1

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"},
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q0"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/q1"},
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")
        self._create_replica_tables(replicas, replica_ids)

        cypress_replicas = get("//tmp/crt/@replicas")
        assert all(cypress_replicas[replica_id]["replicated_table_tracker_enabled"] for replica_id in replica_ids)

        wait(lambda: get("#{0}/@mode".format(replica_ids[3])) == "sync")
        self._sync_replication_era(card_id)

        with self.CellsDisabled(clusters=["primary"], tablet_bundles=["default"]):
            wait(lambda: get("#{0}/@mode".format(replica_ids[0])) == "async")
            wait(lambda: get("#{0}/@mode".format(replica_ids[2])) == "async")
            wait(lambda: get("#{0}/@mode".format(replica_ids[1])) == "sync")

            alter_table_replica(replica_ids[0], enable_replicated_table_tracker=False)
            assert not get("//tmp/crt/@replicas/{0}/replicated_table_tracker_enabled".format(replica_ids[0]))

            values = [{"key": 0, "value": "0"}]
            keys = [{"key": 0}]
            self._insistent_insert_rows("//tmp/t", values)
            assert lookup_rows("//tmp/t", keys, replica_consistency="sync") == values

        alter_replication_card(card_id, enable_replicated_table_tracker=False)
        assert not get("//tmp/crt/@replicated_table_options/enable_replicated_table_tracker")
        replicated_table_options["min_sync_replica_count"] = 2
        alter_replication_card(card_id, replicated_table_options=replicated_table_options)
        options = get("//tmp/crt/@replicated_table_options")
        assert options["enable_replicated_table_tracker"]
        assert options["min_sync_replica_count"] == 2
        wait(lambda: get("#{0}/@mode".format(replica_ids[2])) == "sync")
        assert get("#{0}/@mode".format(replica_ids[1])) == "sync"
        assert get("#{0}/@mode".format(replica_ids[3])) == "sync"

        assert get("#{0}/@mode".format(replica_ids[0])) == "async"
        alter_table_replica(replica_ids[0], enable_replicated_table_tracker=True)
        wait(lambda: get("#{0}/@mode".format(replica_ids[0])) == "sync")

    @authors("akozhikhov")
    def test_banned_replica_cluster(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)

        replicated_table_options = {
            "enable_replicated_table_tracker": True,
            "tablet_cell_bundle_name_ttl": 1000,
            "tablet_cell_bundle_name_failure_interval": 100,
        }
        create("chaos_replicated_table", "//tmp/crt", attributes={
            "chaos_cell_bundle": "c",
            "replicated_table_options": replicated_table_options
        })
        card_id = get("//tmp/crt/@replication_card_id")
        options = get("//tmp/crt/@replicated_table_options")
        assert options["enable_replicated_table_tracker"]

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"},
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q0"},
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")
        self._create_replica_tables(replicas, replica_ids)

        for driver in self._get_drivers():
            set("//sys/@config/tablet_manager/replicated_table_tracker/use_new_replicated_table_tracker", True, driver=driver)

        self._sync_replication_era(card_id, replicas)

        for driver in self._get_drivers():
            set("//sys/@config/tablet_manager/replicated_table_tracker/replicator_hint/banned_replica_clusters", ["primary"], driver=driver)

        wait(lambda: get("#{0}/@mode".format(replica_ids[0])) == "async")
        wait(lambda: get("#{0}/@mode".format(replica_ids[1])) == "sync")
        wait(lambda: get("#{0}/@mode".format(replica_ids[2])) == "sync")

    @authors("savrus")
    def test_ordered_replicated_table_tracker(self):
        self._init_replicated_table_tracker()
        cell_id = self._sync_create_chaos_bundle_and_cell()
        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)

        replicated_table_options = {
            "enable_replicated_table_tracker": True,
            "tablet_cell_bundle_name_ttl": 1000,
            "tablet_cell_bundle_name_failure_interval": 100,
        }
        create("chaos_replicated_table", "//tmp/crt", attributes={
            "chaos_cell_bundle": "c",
            "replicated_table_options": replicated_table_options
        })
        card_id = get("//tmp/crt/@replication_card_id")

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/r"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")
        self._create_replica_tables(replicas, replica_ids, ordered=True)

        for driver in self._get_drivers():
            set("//sys/@config/tablet_manager/replicated_table_tracker/replicator_hint/banned_replica_clusters", ["primary"], driver=driver)
        wait(lambda: get("#{0}/@mode".format(replica_ids[0])) == "async")
        wait(lambda: get("#{0}/@mode".format(replica_ids[1])) == "sync")
        wait(lambda: get("#{0}/@mode".format(replica_ids[2])) == "sync")
        self._sync_replication_era(card_id)

        data_values = [{"key": i, "value": str(i)} for i in range(1)]
        values = [{"$tablet_index": 0, "key": i, "value": str(i)} for i in range(1)]
        insert_rows("//tmp/t", values)

        for replica in replicas:
            wait(lambda: select_rows("key, value from [{0}]".format(replica["replica_path"]), driver=get_driver(cluster=replica["cluster_name"])) == data_values)

    @authors("savrus")
    @pytest.mark.parametrize("snapshotting", ["none", "snapshot"])
    @pytest.mark.parametrize("migration", ["none", "migrate"])
    @pytest.mark.parametrize("max_sync", [1, 2])
    def test_replication_card_collocation(self, max_sync, migration, snapshotting):
        self._init_replicated_table_tracker()
        cell_id = self._sync_create_chaos_bundle_and_cell()
        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)
        cells = [cell_id]

        replicated_table_options = {
            "enable_replicated_table_tracker": False,
            "min_sync_replica_count": 1,
            "max_sync_replica_count": max_sync,
            "tablet_cell_bundle_name_ttl": 1000,
            "tablet_cell_bundle_name_failure_interval": 100,
        }

        def _create_supertable(prefix, clusters, sync_cluster):
            crt = "{0}-crt".format(prefix)
            create("chaos_replicated_table", crt, attributes={
                "chaos_cell_bundle": "c",
                "replicated_table_options": replicated_table_options
            })
            card_id = get("{0}/@replication_card_id".format(crt))

            replicas = [
                {
                    "cluster_name": cluster,
                    "content_type": content_type,
                    "mode": "sync" if cluster == sync_cluster else "async",
                    "enabled": True,
                    "replica_path": "{0}-{1}-{2}".format(prefix, cluster, content_type),
                }
                for content_type in ["data", "queue"]
                for cluster in clusters
            ]
            replica_ids = self._create_chaos_table_replicas(replicas, table_path=crt)
            self._create_replica_tables(replicas, replica_ids)
            self._sync_replication_era(card_id, replicas)
            return crt, card_id, replicas, replica_ids

        clusters = self.get_cluster_names()
        crt1, card1, replicas1, replica_ids1 = _create_supertable("//tmp/a", clusters, clusters[0])
        crt2, card2, replicas2, replica_ids2 = _create_supertable("//tmp/b", clusters, clusters[1])

        collocation_id = create("replication_card_collocation", None, attributes={
            "type": "replication",
            "table_paths": [crt1, crt2]
        })
        for crt in [crt1, crt2]:
            assert get("{0}/@replication_collocation_id".format(crt)) == collocation_id

        def _get_orchid(cell_id, path):
            address = get("#{0}/@peers/0/address".format(cell_id))
            return get("//sys/cluster_nodes/{0}/orchid/chaos_cells/{1}{2}".format(address, cell_id, path))
        collocation = _get_orchid(cell_id, "/chaos_manager/replication_card_collocations/{0}".format(collocation_id))
        assert collocation["state"] == "normal"
        assert collocation["size"] == 2

        if migration == "migrate":
            dst_cell_id = self._sync_create_chaos_cell()
            cells.append(dst_cell_id)

            with raises_yt_error("Trying to move incomplete collocation"):
                migrate_replication_cards(cell_id, [card1], destination_cell_id=dst_cell_id)

            def _sync_migrate_replication_cards(cell_id, card_ids, destination_cell_id):
                migrate_replication_cards(cell_id, card_ids, destination_cell_id=destination_cell_id)

                def _get_orchid_path(cell_id):
                    address = get("#{0}/@peers/0/address".format(cell_id))
                    return "//sys/cluster_nodes/{0}/orchid/chaos_cells/{1}".format(address, cell_id)

                cell_orchid_path = _get_orchid_path(cell_id)
                dst_orchid_path = _get_orchid_path(destination_cell_id)

                for card_id in card_ids:
                    migration_path = "{0}/chaos_manager/replication_cards/{1}/state".format(cell_orchid_path, card_id)
                    wait(lambda: get(migration_path) == "migrated")

                    migrated_card_path = "{0}/chaos_manager/replication_cards/{1}".format(dst_orchid_path, card_id)
                    wait(lambda: exists(migrated_card_path))

            _sync_migrate_replication_cards(cell_id, [card1, card2], destination_cell_id=dst_cell_id)

            self._sync_replication_era(card1, replicas1)
            self._sync_replication_era(card2, replicas2)

            options = get("{0}/@replicated_table_options".format(crt1))
            assert not options["enable_replicated_table_tracker"]
            assert options["max_sync_replica_count"] == max_sync

            replicas = get("{0}/@replicas".format(crt1))
            assert all(replica["replicated_table_tracker_enabled"] for replica in replicas.values())

        alter_replication_card(card1, enable_replicated_table_tracker=True)
        alter_replication_card(card2, enable_replicated_table_tracker=True)

        if snapshotting:
            _, remote_driver0, remote_driver1 = self._get_drivers()
            for cell in cells:
                def _get_snapshots(cell):
                    return [
                        len(ls("//sys/chaos_cells/{0}/{1}/snapshots".format(cell, index), driver=driver))
                        for index, driver in enumerate(self._get_drivers())
                    ]
                snapshots = _get_snapshots(cell)
                build_snapshot(cell_id=cell)
                wait(lambda: all(new > old for new, old in zip(_get_snapshots(cell), snapshots)))
            with self.CellsDisabled(clusters=self.get_cluster_names(), chaos_bundles=["c"]):
                pass

        def _get_sync_replica_clusters(crt, content_type):
            replicas = get("{0}/@replicas".format(crt))
            valid = lambda replica: replica["mode"] == "sync" and replica["content_type"] == content_type
            return list(builtins.set(replica["cluster_name"] for replica in replicas.values() if valid(replica)))

        def _check(content_type):
            sync1 = _get_sync_replica_clusters(crt1, content_type)
            sync2 = _get_sync_replica_clusters(crt2, content_type)
            sync1.sort()
            sync2.sort()
            import logging
            logger = logging.getLogger()
            logger.debug("Comparing sync {0} replicas: {1} vs {2}".format(content_type, sync1, sync2))
            return sync1 == sync2 and len(sync1) == [2, max_sync][content_type == "data"]
        wait(lambda: _check("data"))
        wait(lambda: _check("queue"))

        for content_type in ["data", "queue"]:
            sync_clusters = _get_sync_replica_clusters(crt1, content_type)
            with self.CellsDisabled(clusters=[sync_clusters[0]], tablet_bundles=["default"]):
                wait(lambda: _get_sync_replica_clusters(crt1, content_type) != sync_clusters)
                wait(lambda: _check(content_type))

    @authors("savrus")
    @pytest.mark.parametrize("method", ["alter", "remove"])
    def test_replication_card_collocation_removed(self, method):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)

        def _create(prefix):
            crt = "{0}-crt".format(prefix)
            create("chaos_replicated_table", crt, attributes={"chaos_cell_bundle": "c"})
            card_id = get("{0}/@replication_card_id".format(crt))
            return crt, card_id

        crt1, card1 = _create("//tmp/a")
        crt2, card2 = _create("//tmp/b")

        collocation_id = create("replication_card_collocation", None, attributes={
            "type": "replication",
            "table_paths": [crt1, crt2]
        })

        def _get_orchid_path(cell_id, path):
            address = get("#{0}/@peers/0/address".format(cell_id))
            return "//sys/cluster_nodes/{0}/orchid/chaos_cells/{1}{2}".format(address, cell_id, path)
        collocation_path = _get_orchid_path(cell_id, "/chaos_manager/replication_card_collocations")
        assert len(get("{0}/{1}/replication_card_ids".format(collocation_path, collocation_id))) == 2

        def _unbind(crt, card):
            if method == "remove":
                remove(crt)
            else:
                alter_replication_card(card, replication_card_collocation_id="0-0-0-0")

        _unbind(crt1, card1)
        wait(lambda: get("{0}/{1}/replication_card_ids".format(collocation_path, collocation_id)) == [card2])
        _unbind(crt2, card2)
        wait(lambda: len(get(collocation_path)) == 0)

    @authors("savrus")
    @pytest.mark.parametrize("reshard", [True, False])
    @pytest.mark.parametrize("first", ["chaos", "replicated"])
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_switch_to_replicated_table(self, mode, first, reshard):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)
        schema = yson.YsonList([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ])
        create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "c", "schema": schema})
        card_id = get("//tmp/crt/@replication_card_id")

        replicas = [
            {"cluster_name": "remote_0", "content_type": "data", "mode": mode, "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/r"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"}
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")

        data_replicas = []
        data_replica_ids = []
        for replica, replica_id in zip(replicas, replica_ids):
            if replica["content_type"] == "data":
                data_replicas.append(replica)
                data_replica_ids.append(replica_id)

        sync_create_cells(1)
        create("replicated_table", "//tmp/rt", attributes={"schema": schema, "dynamic": True})
        if reshard:
            reshard_table("//tmp/rt", [[], [3], [6]])
        sync_mount_table("//tmp/rt")
        rt_replica_ids = []
        for replica in data_replicas:
            attributes = {"mode": replica["mode"]}
            replica_id = create_table_replica("//tmp/rt", replica["cluster_name"], replica["replica_path"], attributes=attributes)
            sync_enable_table_replica(replica_id)
            rt_replica_ids.append(replica_id)

        self._create_replica_tables(replicas, replica_ids, mount_tables=False)
        for replica in replicas:
            driver = get_driver(cluster=replica["cluster_name"])
            path = replica["replica_path"]
            if reshard:
                reshard_table(path, [[], [5]], driver=driver)
            if replica["content_type"] == "queue":
                sync_mount_table(path, driver=driver)

        def _check(keys, values):
            for replica in data_replicas:
                driver = get_driver(cluster=replica["cluster_name"])
                path = replica["replica_path"]
                if replica["mode"] == "sync":
                    assert lookup_rows(path, keys, driver=driver) == values
                else:
                    wait(lambda: lookup_rows(path, keys, driver=driver) == values)

        def _switch(new_replica_ids):
            for replica, new_id in zip(data_replicas, new_replica_ids):
                driver = get_driver(cluster=replica["cluster_name"])
                path = replica["replica_path"]
                sync_unmount_table(path, driver=driver)
                alter_table(path, upstream_replica_id=new_id, driver=driver)
                sync_mount_table(path, driver=driver)

        values = None
        keys = [{"key": i} for i in range(10)]

        for iteration in range(3):
            begin = 0 if first == "chaos" else 1
            if iteration % 2 == begin:
                _switch(data_replica_ids)
                self._sync_replication_era(card_id, replicas)
                table = "//tmp/crt"
            else:
                _switch(rt_replica_ids)
                table = "//tmp/rt"

            new_values = [{"key": i, "value": str(i + iteration)} for i in range(0, 10, iteration + 1)]
            if values is None:
                values = new_values
            else:
                for new_value in new_values:
                    for value in values:
                        if value["key"] == new_value["key"]:
                            value["value"] = new_value["value"]

            insert_rows(table, new_values)
            _check(keys, values)

    @authors("savrus")
    @pytest.mark.parametrize("tablet_count", [1, 2])
    @pytest.mark.parametrize("first", ["chaos", "replicated"])
    @pytest.mark.parametrize("mode", ["sync", "async"])
    def test_ordered_switch_to_replicated_table(self, mode, first, tablet_count):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        set("//sys/chaos_cell_bundles/c/@metadata_cell_id", cell_id)
        schema = yson.YsonList([
            {"name": "key", "type": "int64"},
            {"name": "value", "type": "string"},
        ])
        create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "c", "schema": schema})
        card_id = get("//tmp/crt/@replication_card_id")

        replicas = [
            {"cluster_name": "remote_0", "content_type": "queue", "mode": mode, "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r"},
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")

        sync_create_cells(1)
        create("replicated_table", "//tmp/rt", attributes={"schema": schema, "dynamic": True, "preserve_tablet_index": True})
        if tablet_count > 1:
            reshard_table("//tmp/rt", tablet_count)
        sync_mount_table("//tmp/rt")
        rt_replica_ids = []
        for replica in replicas:
            attributes = {"mode": replica["mode"]}
            replica_id = create_table_replica("//tmp/rt", replica["cluster_name"], replica["replica_path"], attributes=attributes)
            sync_enable_table_replica(replica_id)
            rt_replica_ids.append(replica_id)

        self._create_replica_tables(replicas, replica_ids, mount_tables=False, ordered=True)
        if tablet_count > 1:
            for replica in replicas:
                driver = get_driver(cluster=replica["cluster_name"])
                path = replica["replica_path"]
                reshard_table(path, tablet_count, driver=driver)

        def _check(tablet_index, values):
            import logging
            logger = logging.getLogger()
            logger.debug("Expected: {0}".format(values))

            for replica in replicas:
                driver = get_driver(cluster=replica["cluster_name"])
                path = replica["replica_path"]
                query = "key, value from [{0}] where [$tablet_index] = {1}". format(path, tablet_index)
                wait(lambda: select_rows(query, driver=driver) == values)

        def _switch(new_replica_ids, progress=None):
            for replica, new_id in zip(replicas, new_replica_ids):
                driver = get_driver(cluster=replica["cluster_name"])
                path = replica["replica_path"]
                sync_unmount_table(path, driver=driver)
                alter_table(path, upstream_replica_id=new_id, driver=driver)
                if progress:
                    alter_table(path, replication_progress=progress, driver=driver)
                sync_mount_table(path, driver=driver)

        values = [[] for i in range(tablet_count)]

        for iteration in range(3):
            if (iteration % 2 == 0) == (first == "chaos"):
                timestamp = generate_timestamp()
                progress = {
                    "segments": [{"lower_key": [], "timestamp": timestamp}],
                    "upper_key": [yson.to_yson_type(None, attributes={"type": "max"})]
                }
                _switch(replica_ids, progress)
                self._sync_replication_era(card_id, replicas)
                table = "//tmp/crt"
            else:
                _switch(rt_replica_ids)
                table = "//tmp/rt"

            new_values = [{"$tablet_index": i % tablet_count, "key": i, "value": str(i + iteration)} for i in range(0, 10, iteration + 1)]
            for value in new_values:
                values[value["$tablet_index"]].append({"key": value["key"], "value": value["value"]})

            import logging
            logger = logging.getLogger()
            logger.debug("Inserting: {0}".format(new_values))

            insert_rows(table, new_values)
            for tablet_index in range(tablet_count):
                _check(tablet_index, values[tablet_index])

    @authors("savrus")
    @pytest.mark.parametrize("schemas", [
        ("sorted_simple", "sorted_value2"),
        ("sorted_simple", "sorted_key2"),
    ])
    @pytest.mark.parametrize("tablet_count", [1, 3])
    @pytest.mark.parametrize("mode", ["sync", "async", "async_queue"])
    def test_schema_compatibility(self, schemas, tablet_count, mode):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        enabled = mode != "async_queue"
        replica_mode = "sync" if mode == "sync" else "async"

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": replica_mode, "enabled": enabled, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": replica_mode, "enabled": False, "replica_path": "//tmp/q"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, create_replica_tables=False, sync_replication_era=False)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        pivot_keys = [[]] + [[i] for i in range(1, tablet_count)]
        schema1, schema2 = self._get_schemas_by_name(schemas)
        self._create_replica_tables(replicas[:2], replica_ids[:2], schema=schema2, pivot_keys=pivot_keys)
        self._create_replica_tables(replicas[2:], replica_ids[2:], schema=schema1, pivot_keys=pivot_keys)

        self._sync_alter_replica(card_id, replicas, replica_ids, 1, enabled=True)

        values = [{"key": i, "value": str(i)} for i in range(4)]
        keys = [{"key": i} for i in range(4)]
        insert_rows("//tmp/r", values, driver=remote_driver1, allow_missing_key_columns=True)

        extra_columns = list(builtins.set([c["name"] for c in schema2]) - builtins.set([c["name"] for c in schema1]))
        expected = deepcopy(values)
        for value in expected:
            value.update({c: None for c in extra_columns})

        if mode == "async_queue":
            self._sync_alter_replica(card_id, replicas, replica_ids, 1, mode="sync")
            self._sync_alter_replica(card_id, replicas, replica_ids, 2, enabled=False)
            self._sync_alter_replica(card_id, replicas, replica_ids, 0, enabled=True)

        wait(lambda: lookup_rows("//tmp/t", keys) == expected)

    @authors("savrus")
    @pytest.mark.parametrize("schemas", [
        ("sorted_value2", "sorted_simple"),
        ("sorted_key2", "sorted_simple"),
        ("sorted_simple", "sorted_hash"),
        ("sorted_hash", "sorted_simple"),
        ("sorted_key2", "sorted_key2_inverted")
    ])
    def test_schema_incompatibility(self, schemas):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/r"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, create_replica_tables=False, sync_replication_era=False)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        schema1, schema2 = self._get_schemas_by_name(schemas)
        self._create_replica_tables(replicas[:1], replica_ids[:1], schema=schema2)
        self._create_replica_tables(replicas[1:], replica_ids[1:], schema=schema1)
        self._sync_replication_era(card_id, replicas)

        def _create_data(schema_name):
            if schema_name == "sorted_key2":
                return [{"key": 0, "key2": 0, "value": str(0)}], [{"key": 0, "key2": 0}]
            else:
                return [{"key": 0, "value": str(0)}], [{"key": 0}]

        values, keys = _create_data(schemas[0])
        insert_rows("//tmp/r", values, driver=remote_driver1)

        def _check():
            tablet_infos = get_tablet_infos("//tmp/t", [0], request_errors=True)
            errors = tablet_infos["tablets"][0]["tablet_errors"]
            if len(errors) == 0 or errors[0]["attributes"]["background_activity"] != "pull":
                return False
            message = errors[0]["message"]
            if message.startswith("Table schemas are incompatible"):
                return True
            return False
        wait(_check)

        def are_keys_compatible():
            for index in range(len(schema2)):
                c2 = schema2[index]
                if "sort_order" not in c2:
                    break
                if index == len(schema1):
                    return False
                c1 = schema1[index]
                return c1["name"] == c2["name"] and c1["type"] == c2["type"]

        if not are_keys_compatible():
            with raises_yt_error("Table schemas are incompatible"):
                lookup_rows("//tmp/t", keys, replica_consistency="sync")

    @authors("savrus")
    @pytest.mark.parametrize("schemas", [
        ("ordered_value2", "ordered_simple"),
        ("ordered_simple", "ordered_simple_int"),
    ])
    def test_ordered_schema_incompatibility(self, schemas):
        cell_id = self._sync_create_chaos_bundle_and_cell()

        replicas = [
            {"cluster_name": "primary", "content_type": "queue", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas, create_replica_tables=False, sync_replication_era=False)
        _, remote_driver0, remote_driver1 = self._get_drivers()

        schema1, schema2 = self._get_schemas_by_name(schemas)
        self._create_replica_tables(replicas[:1], replica_ids[:1], schema=schema2, ordered=True)
        self._create_replica_tables(replicas[1:], replica_ids[1:], schema=schema1, ordered=True)
        self._sync_replication_era(card_id, replicas)

        values = [{"$tablet_index": 0, "key": 0, "value": str(0)}]
        insert_rows("//tmp/r", values, driver=remote_driver1)

        def _check():
            tablet_infos = get_tablet_infos("//tmp/t", [0], request_errors=True)
            errors = tablet_infos["tablets"][0]["tablet_errors"]
            if len(errors) == 0 or errors[0]["attributes"]["background_activity"] != "pull":
                return False
            message = errors[0]["message"]
            if message.startswith("Table schemas are incompatible"):
                return True
            return False
        wait(_check)


##################################################################


class TestChaosRpcProxy(TestChaos):
    DRIVER_BACKEND = "rpc"
    ENABLE_RPC_PROXY = True

    @authors("savrus")
    def test_insert_first(self):
        cell_id = self._sync_create_chaos_bundle_and_cell()
        _, remote_driver0, remote_driver1 = self._get_drivers()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"}
        ]
        self._create_chaos_tables(cell_id, replicas)

        with Restarter(self.Env, RPC_PROXIES_SERVICE):
            pass

        def _check():
            try:
                return exists("/")
            except YtError as err:
                print_debug(err)
                return False

        wait(_check)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)

        assert lookup_rows("//tmp/t", [{"key": 0}]) == values
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 0}], driver=remote_driver1) == values)


##################################################################


class TestChaosMulticell(TestChaos):
    NUM_SECONDARY_MASTER_CELLS = 2


##################################################################


class TestChaosMetaCluster(ChaosTestBase):
    NUM_REMOTE_CLUSTERS = 3

    def _create_dedicated_areas_and_cells(self, name="c"):
        align_chaos_cell_tag()

        cluster_names = self.get_cluster_names()
        meta_cluster_names = cluster_names[:-2] + cluster_names[-1:]
        peer_cluster_names = cluster_names[-2:-1]
        create_chaos_cell_bundle(
            name,
            peer_cluster_names,
            meta_cluster_names=meta_cluster_names,
            independent_peers=False)
        default_cell_id = self._sync_create_chaos_cell(
            name=name,
            peer_cluster_names=peer_cluster_names,
            meta_cluster_names=meta_cluster_names)

        meta_cluster_names = cluster_names[:-1]
        peer_cluster_names = cluster_names[-1:]
        create_chaos_area(
            "beta",
            name,
            peer_cluster_names,
            meta_cluster_names=meta_cluster_names)
        beta_cell_id = self._sync_create_chaos_cell(
            name=name,
            peer_cluster_names=peer_cluster_names,
            meta_cluster_names=meta_cluster_names,
            area="beta")

        return [default_cell_id, beta_cell_id]

    @authors("babenko")
    def test_meta_cluster(self):
        cluster_names = self.get_cluster_names()

        peer_cluster_names = cluster_names[1:]
        meta_cluster_names = [cluster_names[0]]

        cell_id = self._sync_create_chaos_bundle_and_cell(peer_cluster_names=peer_cluster_names, meta_cluster_names=meta_cluster_names)

        card_id = create_replication_card(chaos_cell_id=cell_id)
        replicas = [
            {"cluster_name": "remote_0", "content_type": "data", "mode": "sync", "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "queue", "mode": "sync", "replica_path": "//tmp/r1"},
            {"cluster_name": "remote_2", "content_type": "data", "mode": "async", "replica_path": "//tmp/r2"}
        ]
        self._create_chaos_table_replicas(replicas, replication_card_id=card_id)

        card = get("#{0}/@".format(card_id))
        assert card["type"] == "replication_card"
        assert card["id"] == card_id
        assert len(card["replicas"]) == 3

    @authors("savrus")
    def test_dedicated_areas(self):
        cells = self._create_dedicated_areas_and_cells()
        drivers = self._get_drivers()

        def _is_alien(cell, driver):
            return "alien" in get("//sys/chaos_cells/{0}/@peers/0".format(cell), driver=driver)

        assert _is_alien(cells[0], drivers[-1])
        assert _is_alien(cells[1], drivers[-2])
        assert not _is_alien(cells[0], drivers[-2])
        assert not _is_alien(cells[1], drivers[-1])

        areas = get("//sys/chaos_cell_bundles/c/@areas")
        assert len(areas) == 2
        assert all(area["cell_count"] == 1 for area in areas.values())

        def _get_coordinators(cell, driver):
            peer = get("//sys/chaos_cells/{0}/@peers/0/address".format(cell), driver=driver)
            return get("//sys/cluster_nodes/{0}/orchid/chaos_cells/{1}/chaos_manager/coordinators".format(peer, cell), driver=driver)

        assert len(_get_coordinators(cells[0], drivers[-2])) == 2
        assert len(_get_coordinators(cells[1], drivers[-1])) == 2

    @authors("savrus")
    def test_dedicated_chaos_table(self):
        cells = self._create_dedicated_areas_and_cells()
        drivers = self._get_drivers()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"}
        ]
        self._create_chaos_tables(cells[0], replicas)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)

        assert lookup_rows("//tmp/t", [{"key": 0}]) == values
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 0}], driver=drivers[2]) == values)

    @authors("savrus")
    def test_replication_card_migration(self):
        cells = self._create_dedicated_areas_and_cells()
        drivers = self._get_drivers()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
            {"cluster_name": "remote_1", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/r1"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cells[0], replicas)

        migrate_replication_cards(cells[0], [card_id])

        def _get_orchid_path(cell_id, driver=None):
            address = get("#{0}/@peers/0/address".format(cell_id), driver=driver)
            return "//sys/cluster_nodes/{0}/orchid/chaos_cells/{1}".format(address, cell_id)

        migration_path = "{0}/chaos_manager/replication_cards/{1}/state".format(_get_orchid_path(cells[0], driver=drivers[-2]), card_id)
        wait(lambda: get(migration_path, driver=drivers[-2]) == "migrated")

        migrated_card_path = "{0}/chaos_manager/replication_cards/{1}".format(_get_orchid_path(cells[1], driver=drivers[-1]), card_id)
        wait(lambda: exists(migrated_card_path, driver=drivers[-1]))

        self._sync_replication_era(card_id, replicas)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)
        assert lookup_rows("//tmp/t", [{"key": 0}]) == values
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 0}], driver=drivers[2]) == values)

        migrate_replication_cards(cells[1], [card_id])
        wait(lambda: get(migration_path, driver=drivers[-2]) == "normal")
        wait(lambda: get("{0}/state".format(migrated_card_path), driver=drivers[-1]) == "migrated")

        self._sync_replication_era(card_id, replicas)

        values = [{"key": 1, "value": "1"}]
        insert_rows("//tmp/t", values)
        assert lookup_rows("//tmp/t", [{"key": 1}]) == values
        wait(lambda: lookup_rows("//tmp/r1", [{"key": 1}], driver=drivers[2]) == values)

    @authors("savrus")
    def test_remove_migrated_replication_card(self):
        cells = self._create_dedicated_areas_and_cells()
        drivers = self._get_drivers()

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "sync", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/r0"},
        ]
        card_id, replica_ids = self._create_chaos_tables(cells[0], replicas, create_replica_tables=False, sync_replication_era=False)
        self._sync_replication_card(card_id)

        migrate_replication_cards(cells[0], [card_id])

        def _get_orchid_path(cell_id, driver=None):
            address = get("#{0}/@peers/0/address".format(cell_id), driver=driver)
            return "//sys/cluster_nodes/{0}/orchid/chaos_cells/{1}".format(address, cell_id)

        migration_path = "{0}/chaos_manager/replication_cards/{1}/state".format(_get_orchid_path(cells[0], driver=drivers[-2]), card_id)
        wait(lambda: get(migration_path, driver=drivers[-2]) == "migrated")

        migrated_card_path = "{0}/chaos_manager/replication_cards/{1}".format(_get_orchid_path(cells[1], driver=drivers[-1]), card_id)
        wait(lambda: exists(migrated_card_path, driver=drivers[-1]))

        area_id = get("#{0}/@area_id".format(cells[0]), driver=drivers[-2])
        set("#{0}/@node_tag_filter".format(area_id), "invalid", driver=drivers[-2])

        remove("#{0}".format(card_id))
        with pytest.raises(YtError):
            exists("#{0}".format(card_id))

        set("#{0}/@node_tag_filter".format(area_id), "", driver=drivers[-2])
        cluster_names = self.get_cluster_names()
        wait_for_chaos_cell(cells[0], cluster_names[-2:-1])

        def _check():
            try:
                return not exists("#{0}".format(card_id))
            except YtError as err:
                print_debug("Checking if replication card {0} exists failed".format(card_id), err)
                return False
        wait(_check)


##################################################################


class TestChaosMetaClusterRpcProxy(TestChaosMetaCluster):
    DRIVER_BACKEND = "rpc"
    ENABLE_RPC_PROXY = True


##################################################################


class ChaosClockBase(ChaosTestBase):
    NUM_REMOTE_CLUSTERS = 1
    USE_PRIMARY_CLOCKS = False

    DELTA_NODE_CONFIG = {
        "tablet_node": {
            "transaction_manager": {
                "reject_incorrect_clock_cluster_tag": True
            }
        }
    }

    def _create_single_peer_chaos_cell(self, name="c", clock_cluster_tag=None):
        cluster_names = self.get_cluster_names()
        peer_cluster_names = cluster_names[:1]
        meta_cluster_names = cluster_names[1:]
        cell_id = self._sync_create_chaos_bundle_and_cell(
            name=name,
            peer_cluster_names=peer_cluster_names,
            meta_cluster_names=meta_cluster_names,
            clock_cluster_tag=clock_cluster_tag)
        return cell_id


##################################################################


class TestChaosClock(ChaosClockBase):
    @authors("savrus")
    def test_invalid_clock(self):
        drivers = self._get_drivers()
        for driver in drivers:
            clock_cluster_tag = get("//sys/@primary_cell_tag", driver=driver)
            set("//sys/tablet_cell_bundles/default/@options/clock_cluster_tag", clock_cluster_tag, driver=driver)

        cell_id = self._create_single_peer_chaos_cell()
        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"}
        ]
        self._create_chaos_tables(cell_id, replicas)

        with pytest.raises(YtError, match="Transaction timestamp is generated from unexpected clock"):
            insert_rows("//tmp/t", [{"key": 0, "value": "0"}])

    @authors("savrus")
    @pytest.mark.parametrize("mode", ["sync", "async"])
    @pytest.mark.parametrize("primary", [True, False])
    def test_different_clock(self, primary, mode):
        drivers = self._get_drivers()
        clock_cluster_tag = get("//sys/@primary_cell_tag", driver=drivers[0 if primary else 1])
        for driver in drivers:
            set("//sys/tablet_cell_bundles/default/@options/clock_cluster_tag", clock_cluster_tag, driver=driver)

        cell_id = self._create_single_peer_chaos_cell(clock_cluster_tag=clock_cluster_tag)
        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": mode, "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"}
        ]
        card_id, replica_ids = self._create_chaos_tables(cell_id, replicas)

        def _run_iterations():
            total_iterations = 10
            for iteration in range(total_iterations):
                rows = [{"key": 1, "value": str(iteration)}]
                keys = [{"key": 1}]
                if primary:
                    insert_rows("//tmp/t", rows)
                else:
                    insert_rows("//tmp/q", rows, driver=drivers[1])
                wait(lambda: lookup_rows("//tmp/t", keys) == rows)

                if iteration < total_iterations - 1:
                    mode = ["sync", "async"][iteration % 2]
                    self._sync_alter_replica(card_id, replicas, replica_ids, 0, mode=mode)
        _run_iterations()

        # Check that master transactions are working.
        sync_flush_table("//tmp/t")
        sync_flush_table("//tmp/q", driver=drivers[1])
        _run_iterations()


##################################################################


class TestChaosClockRpcProxy(ChaosClockBase):
    DRIVER_BACKEND = "rpc"
    ENABLE_RPC_PROXY = True

    def _set_proxies_clock_cluster_tag(self, clock_cluster_tag, driver=None):
        config = {
            "cluster_connection": {
                "clock_manager": {
                    "clock_cluster_tag": clock_cluster_tag,
                },
                # FIXME(savrus) Workaround for YT-16713.
                "table_mount_cache": {
                    "expire_after_successful_update_time": 0,
                    "expire_after_failed_update_time": 0,
                    "expire_after_access_time": 0,
                    "refresh_time": 0,
                },
            },
        }

        set("//sys/rpc_proxies/@config", config, driver=driver)

        proxies = ls("//sys/rpc_proxies")

        def _check_config():
            orchid_path = "orchid/dynamic_config_manager/effective_config/cluster_connection/clock_manager/clock_cluster_tag"
            for proxy in proxies:
                path = "//sys/rpc_proxies/{0}/{1}".format(proxy, orchid_path)
                if not exists(path) or get(path) != clock_cluster_tag:
                    return False
            return True
        wait(_check_config)

    @authors("savrus")
    def test_invalid_clock_source(self):
        drivers = self._get_drivers()
        remote_clock_tag = get("//sys/@primary_cell_tag", driver=drivers[1])
        self._set_proxies_clock_cluster_tag(remote_clock_tag)

        clock_cluster_tag = get("//sys/@primary_cell_tag")
        set("//sys/tablet_cell_bundles/default/@options/clock_cluster_tag", clock_cluster_tag)

        self._create_sorted_table("//tmp/t")
        sync_create_cells(1)
        sync_mount_table("//tmp/t")

        with pytest.raises(YtError, match="Transaction origin clock source differs from coordinator clock source"):
            insert_rows("//tmp/t", [{"key": 0, "value": "0"}])

        with pytest.raises(YtError):
            generate_timestamp()

    @authors("savrus")
    def test_chaos_write_with_client_clock_tag(self):
        drivers = self._get_drivers()
        remote_clock_tag = get("//sys/@primary_cell_tag", driver=drivers[1])
        self._set_proxies_clock_cluster_tag(remote_clock_tag)

        for driver in drivers:
            clock_cluster_tag = get("//sys/@primary_cell_tag", driver=driver)
            set("//sys/tablet_cell_bundles/default/@options/clock_cluster_tag", clock_cluster_tag, driver=driver)

        cell_id = self._create_single_peer_chaos_cell(clock_cluster_tag=remote_clock_tag)
        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"}
        ]
        self._create_chaos_tables(cell_id, replicas)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/t", values)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)

    @authors("savrus")
    def test_chaos_replicated_table_with_different_clock_tag(self):
        drivers = self._get_drivers()
        remote_clock_tag = get("//sys/@primary_cell_tag", driver=drivers[1])
        self._set_proxies_clock_cluster_tag(remote_clock_tag)

        for driver in drivers:
            clock_cluster_tag = get("//sys/@primary_cell_tag", driver=driver)
            set("//sys/tablet_cell_bundles/default/@options/clock_cluster_tag", clock_cluster_tag, driver=driver)

        cell_id = self._create_single_peer_chaos_cell(name="chaos_bundle", clock_cluster_tag=remote_clock_tag)
        set("//sys/chaos_cell_bundles/chaos_bundle/@metadata_cell_id", cell_id)
        schema = yson.YsonList([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ])
        create("chaos_replicated_table", "//tmp/crt", attributes={"chaos_cell_bundle": "chaos_bundle", "schema": schema})

        replicas = [
            {"cluster_name": "primary", "content_type": "data", "mode": "async", "enabled": True, "replica_path": "//tmp/t"},
            {"cluster_name": "remote_0", "content_type": "queue", "mode": "sync", "enabled": True, "replica_path": "//tmp/q"}
        ]
        replica_ids = self._create_chaos_table_replicas(replicas, table_path="//tmp/crt")
        self._create_replica_tables(replicas, replica_ids)
        card_id = get("//tmp/crt/@replication_card_id")
        self._sync_replication_era(card_id, replicas)

        values = [{"key": 0, "value": "0"}]
        insert_rows("//tmp/crt", values)
        wait(lambda: lookup_rows("//tmp/t", [{"key": 0}]) == values)

    @authors("savrus")
    def test_generate_timestamps(self):
        drivers = self._get_drivers()
        remote_clock_tag = get("//sys/@primary_cell_tag", driver=drivers[1])
        self._set_proxies_clock_cluster_tag(remote_clock_tag)

        with raises_yt_error("Unable to generate timestamps: clock source is configured to non-native clock"):
            generate_timestamp()

        config = deepcopy(self.Env.configs["rpc_driver"])
        config["clock_cluster_tag"] = remote_clock_tag
        config["api_version"] = 3

        rpc_driver = Driver(config=config)
        generate_timestamp(driver=rpc_driver)
