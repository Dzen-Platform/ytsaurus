from yt_env_setup import YTEnvSetup, Restarter, MASTERS_SERVICE, NODES_SERVICE
from yt_commands import (
    ls, exists, get, set, authors, print_debug, build_master_snapshots, create, start_transaction,
    remove, wait, create_user, make_ace, copy,
    commit_transaction, create_dynamic_table, sync_mount_table, insert_rows, sync_unmount_table,
    select_rows, lookup_rows, sync_create_cells, wait_for_cells)

from original_tests.yt.yt.tests.integration.tests.master.test_master_snapshots \
    import MASTER_SNAPSHOT_COMPATIBILITY_CHECKER_LIST

from yt.test_helpers import assert_items_equal

import os
import pytest
import yatest.common
import builtins

##################################################################


def check_chunk_locations():
    node_address = ls("//sys/cluster_nodes")[0]

    location_uuids = list(location["location_uuid"] for location in get("//sys/cluster_nodes/{}/@statistics/locations".format(node_address)))
    assert len(location_uuids) > 0

    yield

    assert_items_equal(get("//sys/cluster_nodes/{}/@chunk_locations".format(node_address)).keys(), location_uuids)
    for location_uuid in location_uuids:
        assert exists("//sys/chunk_locations/{}".format(location_uuid))
        assert get("//sys/chunk_locations/{}/@uuid".format(location_uuid)) == location_uuid
        assert get("//sys/chunk_locations/{}/@node_address".format(location_uuid)) == node_address


def check_queue_list():
    create("table", "//tmp/q", attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]})
    create("table", "//tmp/qq", attributes={"dynamic": False, "schema": [{"name": "data", "type": "string"}]})
    create("table", "//tmp/qqq", attributes={"dynamic": True,
                                             "schema": [{"name": "data", "type": "string", "sort_order": "ascending"},
                                                        {"name": "payload", "type": "string"}]})
    create("map_node", "//tmp/mn")
    tx = start_transaction(timeout=60000)
    create("table", "//tmp/qqqq", attributes={"dynamic": True, "schema": [{"name": "data", "type": "string"}]}, tx=tx)

    yield

    assert builtins.set(get("//sys/@queue_agent_object_revisions")["queues"].keys()) == {"//tmp/q"}
    commit_transaction(tx)
    assert builtins.set(get("//sys/@queue_agent_object_revisions")["queues"].keys()) == {"//tmp/q", "//tmp/qqqq"}


def check_portal_entrance_validation():
    set("//sys/@config/cypress_manager/portal_synchronization_period", 1000)
    create_user("u")
    create("map_node", "//tmp/portal_d")
    set("//tmp/portal_d/@acl", [make_ace("deny", ["u"], ["read"])])
    create("portal_entrance", "//tmp/portal_d/p", attributes={"exit_cell_tag": 13})
    create_user("v")
    set("//tmp/portal_d/p/@acl", [make_ace("deny", ["v"], ["read"])])
    portal_exit_effective_acl = get("//tmp/portal_d/p/@effective_acl")
    portal_entrance_effective_acl = get("//tmp/portal_d/p&/@effective_acl")

    assert portal_entrance_effective_acl != portal_exit_effective_acl

    yield

    wait(lambda: get("//tmp/portal_d/p/@annotation_path") == get("//tmp/portal_d/p&/@annotation_path"))
    wait(lambda: get("//tmp/portal_d/p/@effective_acl") == portal_entrance_effective_acl)


def check_hunks():
    def _get_store_chunk_ids(path):
        chunk_ids = get(path + "/@chunk_ids")
        return [chunk_id for chunk_id in chunk_ids if get("#{}/@chunk_type".format(chunk_id)) == "table"]

    def _get_hunk_chunk_ids(path):
        chunk_ids = get(path + "/@chunk_ids")
        return [chunk_id for chunk_id in chunk_ids if get("#{}/@chunk_type".format(chunk_id)) == "hunk"]

    def _is_hunk_root(object_id):
        return get("#{}/@type".format(object_id)) == "chunk_list" and get("#{}/@kind".format(object_id)) == "hunk_root"

    schema = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value", "type": "string", "max_inline_hunk_size": 10},
    ]

    sync_create_cells(1)
    create_dynamic_table("//tmp/t", schema=schema)

    sync_mount_table("//tmp/t")
    keys = [{"key": i} for i in range(10)]
    rows = [{"key": i, "value": "value" + str(i) + "x" * 20} for i in range(10)]
    insert_rows("//tmp/t", rows)
    assert_items_equal(select_rows("* from [//tmp/t]"), rows)
    assert_items_equal(lookup_rows("//tmp/t", keys), rows)
    sync_unmount_table("//tmp/t")

    store_chunk_ids = _get_store_chunk_ids("//tmp/t")
    assert len(store_chunk_ids) == 1
    store_chunk_id = store_chunk_ids[0]

    assert get("#{}/@ref_counter".format(store_chunk_id)) == 1

    hunk_chunk_ids = _get_hunk_chunk_ids("//tmp/t")
    assert len(hunk_chunk_ids) == 1
    hunk_chunk_id = hunk_chunk_ids[0]

    copy("//tmp/t", "//tmp/t_copy")

    assert get("#{}/@ref_counter".format(hunk_chunk_id)) == 2

    main_chunk_list_id = get("//tmp/t/@chunk_list_id")
    tablet_chunk_list_ids = get("#{}/@child_ids".format(main_chunk_list_id))
    assert len(tablet_chunk_list_ids) == 1
    tablet_chunk_list_id = tablet_chunk_list_ids[0]
    tablet_child_ids = get("#{}/@child_ids".format(tablet_chunk_list_id))
    tablet_hunk_chunk_list_ids = [child_id for child_id in tablet_child_ids if _is_hunk_root(child_id)]
    assert len(tablet_hunk_chunk_list_ids) == 1
    tablet_hunk_chunk_list_id = tablet_hunk_chunk_list_ids[0]

    yield

    assert get("#{}/@ref_counter".format(store_chunk_id)) == 2
    assert get("#{}/@ref_counter".format(hunk_chunk_id)) == 3

    assert_items_equal(get("#{}/@owning_nodes".format(store_chunk_id)), ["//tmp/t", "//tmp/t_copy"])
    assert_items_equal(get("#{}/@owning_nodes".format(hunk_chunk_id)), ["//tmp/t", "//tmp/t_copy"])

    assert get("//tmp/t/@chunk_list_id") == main_chunk_list_id
    tablet_chunk_list_id = get("#{}/@child_ids".format(main_chunk_list_id))[0]
    tablet_child_ids = get("#{}/@child_ids".format(tablet_chunk_list_id))
    assert [child_id for child_id in tablet_child_ids if _is_hunk_root(child_id)] == []
    hunk_chunk_list_id = get("//tmp/t/@hunk_chunk_list_id")
    get("#{}/@child_ids".format(hunk_chunk_list_id)) == [tablet_hunk_chunk_list_id]

    for _ in range(100):
        ok = True
        try:
            sync_mount_table("//tmp/t")
            sync_mount_table("//tmp/t_copy")
        except:
            ok = False
        if ok:
            break

    assert_items_equal(select_rows("* from [//tmp/t]"), rows)
    assert_items_equal(lookup_rows("//tmp/t", keys), rows)
    assert_items_equal(select_rows("* from [//tmp/t] where value = \"{}\"".format(rows[0]["value"])), [rows[0]])

    assert_items_equal(lookup_rows("//tmp/t_copy", keys), rows)

    def check_hunk_chunk_location(table):
        hunk_chunk_list_id = get(f"{table}/@hunk_chunk_list_id")
        hunk_tablet_chunk_list_ids = get(f"#{hunk_chunk_list_id}/@child_ids")
        assert len(hunk_tablet_chunk_list_ids) == 1
        hunk_tablet_chunk_list_id = hunk_tablet_chunk_list_ids[0]
        hunk_chunks = get(f"#{hunk_tablet_chunk_list_id}/@child_ids")
        assert hunk_chunks == [hunk_chunk_id]

    check_hunk_chunk_location("//tmp/t")
    check_hunk_chunk_location("//tmp/t_copy")

    remove("//tmp/t")
    remove("//tmp/t_copy")

    wait(lambda: not exists("#{}".format(store_chunk_id)) and not exists("#{}".format(hunk_chunk_id)))


def check_mount_config_attributes():
    create("map_node", "//tmp/mount_config")

    create("table", "//tmp/mount_config/a", attributes={"min_data_ttl": 123})
    create("table", "//tmp/mount_config/b", attributes={"min_data_ttl": "invalid_string_value"})
    create("table", "//tmp/mount_config/c", attributes={
        "min_data_ttl": 123,
        "max_data_ttl": 456,
        "enable_dynamic_store_read": True,
        "foobar": "bazqux",
    })

    create("table", "//tmp/mount_config/d", attributes={
        "dynamic": True,
        "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}, {"name": "value", "type": "string"}],
        "min_data_ttl": 789,
    })

    # Transaction stuff. No one ever sets mount config attributes in transactions, but nevertheless.
    tx = start_transaction(timeout=60000)

    create("table", "//tmp/mount_config/e", attributes={"min_data_ttl": 4}, tx=tx)

    create("table", "//tmp/mount_config/f", attributes={
        "min_data_ttl": 123,
        "max_data_ttl": 456,
    })
    set("//tmp/mount_config/f/@max_data_ttl", 321, tx=tx)

    create("table", "//tmp/mount_config/g")
    set("//tmp/mount_config/g/@max_data_ttl", 555, tx=tx)

    create("table", "//tmp/mount_config/h", attributes={
        "min_data_ttl": 333,
        "max_data_ttl": 444,
    })
    remove("//tmp/mount_config/h/@min_data_ttl", tx=tx)

    yield

    commit_transaction(tx)

    for key in "abcdefgh":
        path = "//tmp/mount_config/" + key
        if key == "c":
            assert get(path + "/@user_attributes") == {
                "foobar": "bazqux",
                "max_data_ttl": 456,
                "min_data_ttl": 123,
            }
            assert_items_equal(get(path + "/@user_attribute_keys"), ["foobar", "max_data_ttl", "min_data_ttl"])
        else:
            assert get(path + "/@user_attributes") == get(path + "/@mount_config")
            assert_items_equal(get(path + "/@user_attribute_keys"), ls(path + "/@mount_config"))

    assert get("//tmp/mount_config/a/@mount_config") == {"min_data_ttl": 123}
    assert get("//tmp/mount_config/b/@mount_config") == {"min_data_ttl": "invalid_string_value"}
    assert get("//tmp/mount_config/c/@mount_config") == {"min_data_ttl": 123, "max_data_ttl": 456}
    assert get("//tmp/mount_config/d/@mount_config") == {"min_data_ttl": 789}

    assert get("//tmp/mount_config/e/@mount_config") == {"min_data_ttl": 4}

    assert builtins.set(get("//tmp/mount_config/f/@mount_config")) == builtins.set(("min_data_ttl", "max_data_ttl"))
    assert get("//tmp/mount_config/f/@mount_config/min_data_ttl") == 123
    # Which of trunk and non-trunk values will have precedence is not specified.
    assert get("//tmp/mount_config/f/@mount_config/max_data_ttl") in (456, 321)

    assert get("//tmp/mount_config/g/@mount_config") == {"max_data_ttl": 555}

    # Attribute removals in a transaction are ignored.
    assert get("//tmp/mount_config/h/@mount_config") == {"min_data_ttl": 333, "max_data_ttl": 444}


class TestMasterSnapshotsCompatibility(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_SECONDARY_MASTER_CELLS = 3
    NUM_NODES = 5
    USE_DYNAMIC_TABLES = True

    DELTA_MASTER_CONFIG = {
        "logging": {
            "abort_on_alert": False,
        },
    }

    DELTA_NODE_CONFIG = {
        "data_node": {
            "incremental_heartbeat_period": 100,
        },
        "cluster_connection": {
            "medium_directory_synchronizer": {
                "sync_period": 1
            }
        }
    }

    ARTIFACT_COMPONENTS = {
        "22_1": ["master"],
        "trunk": ["scheduler", "controller-agent", "proxy", "http-proxy", "node", "job-proxy", "exec", "tools"],
    }

    def teardown_method(self, method):
        master_path = os.path.join(self.bin_path, "ytserver-master")
        if os.path.exists(master_path + "__BACKUP"):
            print_debug("Removing symlink {}".format(master_path))
            os.remove(master_path)
            print_debug("Renaming {} to {}".format(master_path + "__BACKUP", master_path))
            os.rename(master_path + "__BACKUP", master_path)
        super(TestMasterSnapshotsCompatibility, self).teardown_method(method)

    @authors("gritukan", "kvk1920")
    def test(self):
        CHECKER_LIST = [
            check_chunk_locations,
            check_queue_list,
            check_portal_entrance_validation,
            check_hunks,
            check_mount_config_attributes,
        ] + MASTER_SNAPSHOT_COMPATIBILITY_CHECKER_LIST

        checker_state_list = [iter(c()) for c in CHECKER_LIST]
        for s in checker_state_list:
            next(s)

        build_master_snapshots(set_read_only=True)

        with Restarter(self.Env, MASTERS_SERVICE):
            master_path = os.path.join(self.bin_path, "ytserver-master")
            ytserver_all_trunk_path = yatest.common.binary_path("yt/yt/packages/tests_package/ytserver-all")
            print_debug("Renaming {} to {}".format(master_path, master_path + "__BACKUP"))
            os.rename(master_path, master_path + "__BACKUP")
            print_debug("Symlinking {} to {}".format(ytserver_all_trunk_path, master_path))
            os.symlink(ytserver_all_trunk_path, master_path)

        for s in checker_state_list:
            with pytest.raises(StopIteration):
                next(s)


class TestTabletCellsSnapshotsCompatibility(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_SECONDARY_MASTER_CELLS = 3
    NUM_NODES = 5
    USE_DYNAMIC_TABLES = True

    DELTA_MASTER_CONFIG = {
        "logging": {
            "abort_on_alert": False,
        },
    }

    DELTA_NODE_CONFIG = {
        "data_node": {
            "incremental_heartbeat_period": 100,
        },
        "cluster_connection": {
            "medium_directory_synchronizer": {
                "sync_period": 1
            }
        }
    }

    ARTIFACT_COMPONENTS = {
        "22_1": ["master", "node"],
        "trunk": ["scheduler", "controller-agent", "proxy", "http-proxy", "job-proxy", "exec", "tools"],
    }

    def teardown_method(self, method):
        node_path = os.path.join(self.bin_path, "ytserver-node")
        if os.path.exists(node_path + "__BACKUP"):
            print_debug("Removing symlink {}".format(node_path))
            os.remove(node_path)
            print_debug("Renaming {} to {}".format(node_path + "__BACKUP", node_path))
            os.rename(node_path + "__BACKUP", node_path)
        super(TestTabletCellsSnapshotsCompatibility, self).teardown_method(method)

    @authors("aleksandra-zh")
    def test(self):
        cell_ids = sync_create_cells(1)

        with Restarter(self.Env, NODES_SERVICE):
            nodes_path = os.path.join(self.bin_path, "ytserver-node")
            ytserver_all_trunk_path = yatest.common.binary_path("yt/yt/packages/tests_package/ytserver-all")
            print_debug("Renaming {} to {}".format(nodes_path, nodes_path + "__BACKUP"))
            os.rename(nodes_path, nodes_path + "__BACKUP")
            print_debug("Symlinking {} to {}".format(ytserver_all_trunk_path, nodes_path))
            os.symlink(ytserver_all_trunk_path, nodes_path)

        wait_for_cells(cell_ids)
