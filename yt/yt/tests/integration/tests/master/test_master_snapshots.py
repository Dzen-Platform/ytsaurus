from yt_env_setup import YTEnvSetup, Restarter, MASTERS_SERVICE

from yt_helpers import profiler_factory

from yt_commands import (
    authors, wait, create, ls, get, set, copy, remove,
    exists, create_account,
    remove_account, create_proxy_role, create_account_resource_usage_lease, start_transaction, abort_transaction, commit_transaction, lock, insert_rows,
    lookup_rows, alter_table, write_table, wait_for_cells,
    sync_create_cells, sync_mount_table, sync_freeze_table, sync_reshard_table, get_singular_chunk_id,
    get_account_disk_space, create_dynamic_table, build_snapshot,
    build_master_snapshots, clear_metadata_caches, create_pool_tree, create_pool, move, create_medium)

from yt.common import YtError
from yt_type_helpers import make_schema, normalize_schema

from yt.environment.helpers import assert_items_equal

import pytest

from copy import deepcopy
import time

##################################################################


def check_simple_node():
    set("//tmp/a", 42)

    yield

    assert get("//tmp/a") == 42


def check_schema():
    def get_schema(strict):
        return make_schema(
            [{"name": "value", "type": "string", "required": True}],
            unique_keys=False,
            strict=strict,
        )

    create("table", "//tmp/table1", attributes={"schema": get_schema(True)})
    create("table", "//tmp/table2", attributes={"schema": get_schema(True)})
    create("table", "//tmp/table3", attributes={"schema": get_schema(False)})

    yield

    assert normalize_schema(get("//tmp/table1/@schema")) == get_schema(True)
    assert normalize_schema(get("//tmp/table2/@schema")) == get_schema(True)
    assert normalize_schema(get("//tmp/table3/@schema")) == get_schema(False)


def check_forked_schema():
    schema1 = make_schema(
        [{"name": "foo", "type": "string", "required": True}],
        unique_keys=False,
        strict=True,
    )
    schema2 = make_schema(
        [
            {"name": "foo", "type": "string", "required": True},
            {"name": "bar", "type": "string", "required": True},
        ],
        unique_keys=False,
        strict=True,
    )

    create("table", "//tmp/forked_schema_table", attributes={"schema": schema1})
    tx = start_transaction(timeout=60000)
    lock("//tmp/forked_schema_table", mode="snapshot", tx=tx)

    alter_table("//tmp/forked_schema_table", schema=schema2)

    yield

    assert normalize_schema(get("//tmp/forked_schema_table/@schema")) == schema2
    assert normalize_schema(get("//tmp/forked_schema_table/@schema", tx=tx)) == schema1


def check_removed_account():
    create_account("a1")
    create_account("a2")

    for i in range(0, 5):
        table = "//tmp/a1_table{0}".format(i)
        create("table", table, attributes={"account": "a1"})
        write_table(table, {"a": "b"})
        copy(table, "//tmp/a2_table{0}".format(i), attributes={"account": "a2"})

    for i in range(0, 5):
        chunk_id = get_singular_chunk_id("//tmp/a2_table{0}".format(i))
        wait(lambda: len(get("#{0}/@requisition".format(chunk_id))) == 2)

    for i in range(0, 5):
        remove("//tmp/a1_table" + str(i))

    remove_account("a1", sync_deletion=False)

    yield

    for i in range(0, 5):
        chunk_id = get_singular_chunk_id("//tmp/a2_table{0}".format(i))
        wait(lambda: len(get("#{0}/@requisition".format(chunk_id))) == 1)


def check_hierarchical_accounts():
    create_account("b1")
    create_account("b2")
    create_account("b11", "b1")
    create_account("b21", "b2")

    create("table", "//tmp/b11_table", attributes={"account": "b11"})
    write_table("//tmp/b11_table", {"a": "b"})
    create("table", "//tmp/b21_table", attributes={"account": "b21"})
    write_table("//tmp/b21_table", {"a": "b", "c": "d"})
    remove_account("b2", sync_deletion=False)

    # XXX(kiselyovp) this might be flaky
    wait(lambda: get("//sys/accounts/b11/@resource_usage/disk_space_per_medium/default") > 0)
    wait(lambda: get("//sys/accounts/b21/@resource_usage/disk_space_per_medium/default") > 0)
    b11_disk_usage = get("//sys/accounts/b11/@resource_usage/disk_space_per_medium/default")
    b21_disk_usage = get("//sys/accounts/b21/@resource_usage/disk_space_per_medium/default")

    yield

    accounts = ls("//sys/accounts")

    for account in accounts:
        assert not account.startswith("#")

    topmost_accounts = ls("//sys/account_tree")
    for account in [
        "sys",
        "tmp",
        "intermediate",
        "chunk_wise_accounting_migration",
        "b1",
        "b2",
    ]:
        assert account in accounts
        assert account in topmost_accounts
    for account in ["b11", "b21", "root"]:
        assert account in accounts
        assert account not in topmost_accounts

    wait(lambda: get("//sys/account_tree/@ref_counter") == len(topmost_accounts) + 1)

    assert ls("//sys/account_tree/b1") == ["b11"]
    assert ls("//sys/account_tree/b2") == ["b21"]
    assert ls("//sys/account_tree/b1/b11") == []
    assert ls("//sys/account_tree/b2/b21") == []

    assert get("//sys/accounts/b21/@resource_usage/disk_space_per_medium/default") == b21_disk_usage
    assert get("//sys/accounts/b2/@recursive_resource_usage/disk_space_per_medium/default") == b21_disk_usage

    set("//tmp/b21_table/@account", "b11")
    wait(lambda: not exists("//sys/account_tree/b2"), iter=120, sleep_backoff=0.5)
    assert not exists("//sys/accounts/b2")
    assert exists("//sys/accounts/b11")

    assert get("//sys/accounts/b11/@resource_usage/disk_space_per_medium/default") == b11_disk_usage + b21_disk_usage


def check_master_memory():
    resource_limits = {
        "disk_space_per_medium": {"default": 100000},
        "chunk_count": 100,
        "node_count": 100,
        "tablet_count": 100,
        "tablet_static_memory": 100000,
        "master_memory":
        {
            "total": 100000,
            "chunk_host": 100000,
        }
    }
    create_account("a", attributes={"resource_limits": resource_limits})

    create("map_node", "//tmp/dir1", attributes={"account": "a", "sdkjnfkdjs": "lsdkfj"})
    create("table", "//tmp/dir1/t", attributes={"account": "a", "aksdj": "sdkjf"})
    write_table("//tmp/dir1/t", {"adssaa": "kfjhsdkb"})
    copy("//tmp/dir1", "//tmp/dir2", preserve_account=True)

    sync_create_cells(1)
    schema = [
        {"name": "key", "type": "int64", "sort_order": "ascending"},
        {"name": "value", "type": "string"},
    ]
    create_dynamic_table("//tmp/d1", schema=schema, account="a")
    create_dynamic_table("//tmp/d2", schema=schema, account="a")

    sync_reshard_table("//tmp/d1", [[], [1], [2]])
    rows = [{"key": 0, "value": "0"}]
    sync_mount_table("//tmp/d1")
    insert_rows("//tmp/d1", rows)
    sync_freeze_table("//tmp/d1")

    time.sleep(3)

    master_memory_usage = get("//sys/accounts/a/@resource_usage/master_memory")

    yield

    wait(lambda: get("//sys/accounts/a/@resource_usage/master_memory") == master_memory_usage)


def check_dynamic_tables():
    sync_create_cells(1)
    create_dynamic_table(
        "//tmp/table_dynamic",
        schema=[
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ],
    )
    rows = [{"key": 0, "value": "0"}]
    keys = [{"key": 0}]
    sync_mount_table("//tmp/table_dynamic")
    insert_rows("//tmp/table_dynamic", rows)
    sync_freeze_table("//tmp/table_dynamic")

    yield

    clear_metadata_caches()
    wait_for_cells()
    assert lookup_rows("//tmp/table_dynamic", keys) == rows


def check_security_tags():
    for i in range(10):
        create(
            "table",
            "//tmp/table_with_tags" + str(i),
            attributes={"security_tags": ["atag" + str(i), "btag" + str(i)]},
        )

    yield

    for i in range(10):
        assert_items_equal(
            get("//tmp/table_with_tags" + str(i) + "/@security_tags"),
            ["atag" + str(i), "btag" + str(i)],
        )


def check_transactions():
    tx1 = start_transaction(timeout=120000)
    tx2 = start_transaction(tx=tx1, timeout=120000)

    create("portal_entrance", "//tmp/p1", attributes={"exit_cell_tag": 12})
    create("portal_entrance", "//tmp/p2", attributes={"exit_cell_tag": 13})
    create("table", "//tmp/p1/t", tx=tx1)  # replicate tx1 to cell 12
    create("table", "//tmp/p2/t", tx=tx2)  # replicate tx2 to cell 13
    assert get("#{}/@replicated_to_cell_tags".format(tx1)) == [12, 13]
    assert get("#{}/@replicated_to_cell_tags".format(tx2)) == [13]

    yield

    assert get("#{}/@replicated_to_cell_tags".format(tx1)) == [12, 13]
    assert get("#{}/@replicated_to_cell_tags".format(tx2)) == [13]


def check_duplicate_attributes():
    attrs = []
    for i in range(10):
        attrs.append(str(i) * 2000)

    for i in range(10):
        create("map_node", "//tmp/m{}".format(i))
        for j in range(len(attrs)):
            set("//tmp/m{}/@a{}".format(i, j), attrs[j])

    yield

    for i in range(10):
        for j in range(len(attrs)):
            assert get("//tmp/m{}/@a{}".format(i, j)) == attrs[j]


def check_proxy_roles():
    create("http_proxy_role_map", "//sys/http_proxy_roles")
    create("rpc_proxy_role_map", "//sys/rpc_proxy_roles")
    create_proxy_role("h", "http")
    create_proxy_role("r", "rpc")

    yield

    assert get("//sys/http_proxy_roles/h/@proxy_kind") == "http"
    assert get("//sys/rpc_proxy_roles/r/@proxy_kind") == "rpc"


def check_attribute_tombstone_yt_14682():
    create("table", "//tmp/table_with_attr")
    set("//tmp/table_with_attr/@attr", "value")

    tx = start_transaction()
    remove("//tmp/table_with_attr/@attr", tx=tx)

    yield

    assert get("//tmp/table_with_attr/@attr") == "value"
    assert not exists("//tmp/table_with_attr/@attr", tx=tx)


def check_error_attribute():
    cell_id = sync_create_cells(1)[0]
    get("#{}/@peers/0/address".format(cell_id))
    tx_id = get("#{}/@prerequisite_transaction_id".format(cell_id))
    commit_transaction(tx_id)
    wait(lambda: exists("#{}/@peers/0/last_revocation_reason".format(cell_id)))

    yield

    assert exists("#{}/@peers/0/last_revocation_reason".format(cell_id))


def check_account_subtree_size_recalculation():
    set("//sys/@config/security_manager/max_account_subtree_size", 3)
    create_account("b", empty=True)
    create_account("c", empty=True)
    create_account("ca", "c", empty=True)
    create_account("d", empty=True)
    create_account("da", "d", empty=True)
    create_account("db", "d", empty=True)

    yield

    set("//sys/@config/security_manager/max_account_subtree_size", 3)
    with pytest.raises(YtError):
        move("//sys/account_tree/b", "//sys/account_tree/d/dc")
    move("//sys/account_tree/d/db", "//sys/account_tree/c/cb")
    move("//sys/account_tree/b", "//sys/account_tree/d/db")
    with pytest.raises(YtError):
        move("//sys/account_tree/c/ca", "//sys/account_tree/d/dc")
    set("//sys/@config/security_manager/max_account_subtree_size", 4)
    move("//sys/account_tree/c/ca", "//sys/account_tree/d/dc")
    with pytest.raises(YtError):
        move("//sys/account_tree/c", "//sys/account_tree/d/da/daa")
    set("//sys/@config/security_manager/max_account_subtree_size", 6)
    move("//sys/account_tree/c", "//sys/account_tree/d/da/daa")


def check_scheduler_pool_subtree_size_recalculation():
    set("//sys/@config/scheduler_pool_manager/max_scheduler_pool_subtree_size", 3)
    create_pool_tree("tree1", wait_for_orchid=False)
    create_pool_tree("tree2", wait_for_orchid=False)
    create_pool_tree("tree3", wait_for_orchid=False)
    create_pool("a", pool_tree="tree1", wait_for_orchid=False)
    create_pool("aa", pool_tree="tree1", parent_name="a", wait_for_orchid=False)
    create_pool("aaa", pool_tree="tree1", parent_name="aa", wait_for_orchid=False)
    create_pool("b", pool_tree="tree1", wait_for_orchid=False)
    create_pool("ba", pool_tree="tree1", parent_name="b", wait_for_orchid=False)
    create_pool("c", pool_tree="tree1", wait_for_orchid=False)
    create_pool("a", pool_tree="tree2", wait_for_orchid=False)
    create_pool("aa", pool_tree="tree2", parent_name="a", wait_for_orchid=False)

    yield

    set("//sys/@config/scheduler_pool_manager/max_scheduler_pool_subtree_size", 3)
    with pytest.raises(YtError):
        create_pool("aab", pool_tree="tree1", parent_name="aa", wait_for_orchid=False)
    create_pool("bb", pool_tree="tree1", parent_name="b", wait_for_orchid=False)
    with pytest.raises(YtError):
        create_pool("bba", pool_tree="tree1", parent_name="bb", wait_for_orchid=False)
    create_pool("ca", pool_tree="tree1", parent_name="c", wait_for_orchid=False)
    create_pool("cb", pool_tree="tree1", parent_name="c", wait_for_orchid=False)
    with pytest.raises(YtError):
        create_pool("cd", pool_tree="tree1", parent_name="c", wait_for_orchid=False)
    create_pool("aaa", pool_tree="tree2", parent_name="aa", wait_for_orchid=False)
    with pytest.raises(YtError):
        create_pool("aaaa", pool_tree="tree2", parent_name="aaa", wait_for_orchid=False)
    create_pool("a", pool_tree="tree3", wait_for_orchid=False)


def check_account_resource_usage_lease():
    create_account("a42")
    tx = start_transaction()
    lease_id = create_account_resource_usage_lease(account="a42", transaction_id=tx)
    assert exists("#" + lease_id)

    set("#{}/@resource_usage".format(lease_id), {"disk_space_per_medium": {"default": 1000}})
    assert get_account_disk_space("a42") == 1000

    yield

    assert exists("#" + lease_id)
    assert get_account_disk_space("a42") == 1000
    abort_transaction(tx)

    assert not exists("#" + lease_id)
    assert get_account_disk_space("a42") == 0


def check_chunk_locations():
    node_to_location_uuids = {}

    nodes = ls("//sys/cluster_nodes", attributes=["chunk_locations"])
    for node in nodes:
        node_address = str(node)
        location_uuids = node.attributes["chunk_locations"].keys()
        node_to_location_uuids[node_address] = location_uuids

    create_medium("nvme_override")
    overridden_node_address = str(nodes[0])
    overridden_location_uuids = node_to_location_uuids[overridden_node_address]
    for location_uuid in overridden_location_uuids:
        set("//sys/chunk_locations/{}/@medium_override".format(location_uuid), "nvme_override")

    def check_everything():
        for node_address, location_uuids in node_to_location_uuids.items():
            assert exists("//sys/cluster_nodes/{}".format(node_address))
            found_location_uuids = get("//sys/cluster_nodes/{}/@chunk_locations".format(node_address)).keys()
            assert_items_equal(location_uuids, found_location_uuids)

        assert exists("//sys/media/nvme_override")

        for location_uuid in overridden_location_uuids:
            assert get("//sys/chunk_locations/{}/@medium_override".format(location_uuid)) == "nvme_override"

    check_everything()

    yield

    check_everything()


MASTER_SNAPSHOT_CHECKER_LIST = [
    check_simple_node,
    check_schema,
    check_forked_schema,
    check_dynamic_tables,
    check_security_tags,
    check_master_memory,
    check_hierarchical_accounts,
    check_duplicate_attributes,
    check_proxy_roles,
    check_attribute_tombstone_yt_14682,
    check_error_attribute,
    check_account_subtree_size_recalculation,
    check_scheduler_pool_subtree_size_recalculation,
    check_chunk_locations,
    check_account_resource_usage_lease,
    check_removed_account,  # keep this item last as it's sensitive to timings
]

MASTER_SNAPSHOT_COMPATIBILITY_CHECKER_LIST = deepcopy(MASTER_SNAPSHOT_CHECKER_LIST)

# Master memory is a volatile currency, so we do not run compat tests for it.
MASTER_SNAPSHOT_COMPATIBILITY_CHECKER_LIST.remove(check_master_memory)

# Chunk locations API has been changed, no way compat tests could work.
MASTER_SNAPSHOT_COMPATIBILITY_CHECKER_LIST.remove(check_chunk_locations)


class TestMasterSnapshots(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    USE_DYNAMIC_TABLES = True

    @authors("ermolovd")
    def test(self):
        checker_state_list = [iter(c()) for c in MASTER_SNAPSHOT_CHECKER_LIST]
        for s in checker_state_list:
            next(s)

        build_snapshot(cell_id=None)

        with Restarter(self.Env, MASTERS_SERVICE):
            pass

        for s in checker_state_list:
            with pytest.raises(StopIteration):
                next(s)

    @authors("gritukan")
    def test_master_snapshots_free_space_profiling(self):
        master_address = ls("//sys/primary_masters")[0]
        def check_sensor(path):
            sensors = profiler_factory().at_primary_master(master_address).list()
            return path in sensors
        wait(lambda: check_sensor(b"yt/snapshots/free_space"))
        wait(lambda: check_sensor(b"yt/snapshots/available_space"))
        wait(lambda: check_sensor(b"yt/changelogs/free_space"))
        wait(lambda: check_sensor(b"yt/changelogs/available_space"))


class TestAllMastersSnapshots(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    USE_DYNAMIC_TABLES = True
    NUM_SECONDARY_MASTER_CELLS = 3

    @authors("aleksandra-zh")
    def test(self):
        CHECKER_LIST = [
            check_simple_node,
            check_schema,
            check_forked_schema,
            check_dynamic_tables,
            check_security_tags,
            check_master_memory,
            check_hierarchical_accounts,
            check_transactions,
            check_removed_account,  # keep this item last as it's sensitive to timings
        ]

        checker_state_list = [iter(c()) for c in CHECKER_LIST]
        for s in checker_state_list:
            next(s)

        build_master_snapshots()

        with Restarter(self.Env, MASTERS_SERVICE):
            pass

        for s in checker_state_list:
            with pytest.raises(StopIteration):
                next(s)


class TestMastersSnapshotsShardedTx(YTEnvSetup):
    NUM_SECONDARY_MASTER_CELLS = 3
    MASTER_CELL_ROLES = {
        "10": ["cypress_node_host"],
        "11": ["transaction_coordinator"],
        "12": ["chunk_host"],
    }

    @authors("aleksandra-zh")
    def test_reads_in_readonly(self):
        def is_leader_in_readonly(monitoring_prefix, master_list):
            for master in master_list:
                monitoring = get(
                    "{}/{}/orchid/monitoring/hydra".format(monitoring_prefix, master),
                    default=None,
                    suppress_transaction_coordinator_sync=True)
                if monitoring is not None and monitoring["state"] == "leading" and monitoring["read_only"]:
                    return True

            return False

        tx = start_transaction(coordinator_master_cell_tag=11)
        create("map_node", "//tmp/m", tx=tx)

        build_snapshot(cell_id=self.Env.configs["master"][0]["primary_master"]["cell_id"], set_read_only=True)

        primary = ls("//sys/primary_masters", suppress_transaction_coordinator_sync=True)
        wait(lambda: is_leader_in_readonly("//sys/primary_masters", primary))

        abort_transaction(tx)

        for secondary_master in self.Env.configs["master"][0]["secondary_masters"]:
            build_snapshot(cell_id=secondary_master["cell_id"], set_read_only=True)

        secondary_masters = get("//sys/secondary_masters", suppress_transaction_coordinator_sync=True)
        for cell_tag in secondary_masters:
            addresses = list(secondary_masters[cell_tag].keys())
            wait(lambda: is_leader_in_readonly("//sys/secondary_masters/{}".format(cell_tag), addresses))

        # Must not hang on this.
        get("//sys/primary_masters/{}/orchid/monitoring/hydra".format(primary[0]),
            suppress_transaction_coordinator_sync=True)

        with Restarter(self.Env, MASTERS_SERVICE):
            pass

    @authors("aleksandra-zh")
    def test_all_peers_in_readonly(self):
        def all_peers_in_readonly(monitoring_prefix, master_list):
            for master in master_list:
                monitoring = get(
                    "{}/{}/orchid/monitoring/hydra".format(monitoring_prefix, master),
                    default=None,
                    suppress_transaction_coordinator_sync=True)
                if monitoring is None or not monitoring["read_only"]:
                    return False

            return True

        build_master_snapshots(set_read_only=True)

        primary = ls("//sys/primary_masters", suppress_transaction_coordinator_sync=True)
        wait(lambda: all_peers_in_readonly("//sys/primary_masters", primary))

        secondary_masters = get("//sys/secondary_masters", suppress_transaction_coordinator_sync=True)
        for cell_tag in secondary_masters:
            addresses = list(secondary_masters[cell_tag].keys())
            wait(lambda: all_peers_in_readonly("//sys/secondary_masters/{}".format(cell_tag), addresses))

        # Must not hang on this.
        build_master_snapshots(set_read_only=True)

        with Restarter(self.Env, MASTERS_SERVICE):
            pass
