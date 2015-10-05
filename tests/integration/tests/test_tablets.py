import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *
from yt.yson import YsonEntity

from yt.environment.helpers import assert_items_equal

from time import sleep

##################################################################

class TestTablets(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    NUM_SCHEDULERS = 0

    def _create_table(self, path, atomicity="full"):
        create("table", path,
            attributes = {
                "schema": [{"name": "key", "type": "int64"}, {"name": "value", "type": "string"}],
                "key_columns": ["key"],
                "atomicity": atomicity
            })

    def _create_table_with_computed_column(self, path):
        create("table", path,
            attributes = {
                "schema": [
                    {"name": "key1", "type": "int64"},
                    {"name": "key2", "type": "int64", "expression": "key1 * 100 + 3"},
                    {"name": "value", "type": "string"}],
                "key_columns": ["key1", "key2"]
            })

    def _create_table_with_hash(self, path):
        create("table", path,
            attributes = {
                "schema": [
                    {"name": "hash", "type": "uint64", "expression": "farm_hash(key)"},
                    {"name": "key", "type": "int64"},
                    {"name": "value", "type": "string"}],
                "key_columns": ["hash", "key"]
            })

    def _get_tablet_leader_address(self, tablet_id):
        cell_id = get("//sys/tablets/" + tablet_id + "/@cell_id")
        peers = get("//sys/tablet_cells/" + cell_id + "/@peers")
        leader_peer = list(x for x in peers if x["state"] == "leading")[0]
        return leader_peer["address"]

    def _find_tablet_orchid(self, address, tablet_id):
        cells = get("//sys/nodes/" + address + "/orchid/tablet_cells", ignore_opaque=True)
        for (cell_id, cell_data) in cells.iteritems():
            if cell_data["state"] == "leading":
                tablets = cell_data["tablets"]
                if tablet_id in tablets:
                    return tablets[tablet_id]
        return None

    def _get_pivot_keys(self, path):
        tablets = get(path + "/@tablets")
        return [tablet["pivot_key"] for tablet in tablets]
           
    def test_table_cell_bundle(self):
        id = create_tablet_cell_bundle("test_bundle")
        assert ls("//sys/tablet_cell_bundles") == ["test_bundle"]

        test_bundle = get("//sys/tablet_cell_bundles/test_bundle/@")
        assert test_bundle["id"] == id

        remove_tablet_cell_bundle("test_bundle")
        assert ls ("//sys/tablet_cell_bundles") == []

    def test_mount(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")

        mount_table("//tmp/t")
        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 1
        tablet_id = tablets[0]["tablet_id"]
        cell_id = tablets[0]["cell_id"]

        tablet_ids = get("//sys/tablet_cells/" + cell_id + "/@tablet_ids")
        assert tablet_ids == [tablet_id]

    def test_unmount(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")

        mount_table("//tmp/t")

        tablets = get("//tmp/t/@tablets")
        assert len(tablets) == 1

        tablet = tablets[0]
        assert tablet["pivot_key"] == []

        self.sync_mount_table("//tmp/t")
        self.sync_unmount_table("//tmp/t")

    def test_mount_unmount(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        rows = [{"key": 1, "value": "2"}]
        keys = [{"key": 1}]
        insert_rows("//tmp/t", rows)
        actual = lookup_rows("//tmp/t", keys);
        assert actual == rows

        self.sync_unmount_table("//tmp/t")
        with pytest.raises(YtError): lookup_rows("//tmp/t", keys)

        self.sync_mount_table("//tmp/t")
        actual = lookup_rows("//tmp/t", keys);
        assert actual == rows

    def test_reshard_unmounted(self):
        self.sync_create_cells(1, 1)
        create("table", "//tmp/t",
            attributes = {
                "schema": [
                    {"name": "k", "type": "int64"},
                    {"name": "l", "type": "uint64"},
                    {"name": "value", "type": "int64"}],
                "key_columns": ["k", "l"]
            })

        reshard_table("//tmp/t", [[]])
        assert self._get_pivot_keys("//tmp/t") == [[]]

        reshard_table("//tmp/t", [[], [100]])
        assert self._get_pivot_keys("//tmp/t") == [[], [100]]

        with pytest.raises(YtError): reshard_table("//tmp/t", [[], []])
        assert self._get_pivot_keys("//tmp/t") == [[], [100]]

        reshard_table("//tmp/t", [[100], [200]], first_tablet_index=1, last_tablet_index=1)
        assert self._get_pivot_keys("//tmp/t") == [[], [100], [200]]

        with pytest.raises(YtError): reshard_table("//tmp/t", [[101]], first_tablet_index=1, last_tablet_index=1)
        assert self._get_pivot_keys("//tmp/t") == [[], [100], [200]]

        with pytest.raises(YtError): reshard_table("//tmp/t", [[300]], first_tablet_index=3, last_tablet_index=3)
        assert self._get_pivot_keys("//tmp/t") == [[], [100], [200]]

        with pytest.raises(YtError): reshard_table("//tmp/t", [[100], [200]], first_tablet_index=1, last_tablet_index=1)
        assert self._get_pivot_keys("//tmp/t") == [[], [100], [200]]

        reshard_table("//tmp/t", [[100], [150], [200]], first_tablet_index=1, last_tablet_index=2)
        assert self._get_pivot_keys("//tmp/t") == [[], [100], [150], [200]]

        with pytest.raises(YtError): reshard_table("//tmp/t", [[100], [100]], first_tablet_index=1, last_tablet_index=1)
        assert self._get_pivot_keys("//tmp/t") == [[], [100], [150], [200]]

        with pytest.raises(YtError): reshard_table("//tmp/t", [[], [100, 200]])
        assert self._get_pivot_keys("//tmp/t") == [[], [100], [150], [200]]

    def test_force_unmount_on_remove(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        address = self._get_tablet_leader_address(tablet_id)
        assert self._find_tablet_orchid(address, tablet_id) is not None

        remove("//tmp/t")
        sleep(1)
        assert self._find_tablet_orchid(address, tablet_id) is None
         
    def test_read_table(self):
        self.sync_create_cells(1, 1)

        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        rows1 = [{"key": i, "value": str(i)} for i in xrange(10)]
        insert_rows("//tmp/t", rows1)
        self.sync_unmount_table("//tmp/t")

        assert read_table("//tmp/t") == rows1

    def test_read_snapshot_lock(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        def get_chunk_tree(path):
            root_chunk_list_id = get(path + "/@chunk_list_id")
            root_chunk_list = get("#" + root_chunk_list_id + "/@")
            tablet_chunk_lists = [get("#" + x + "/@") for x in root_chunk_list["children_ids"]]
            assert all([root_chunk_list_id in chunk_list["parent_ids"] for chunk_list in tablet_chunk_lists]) 
            return root_chunk_list, tablet_chunk_lists

        def verify_chunk_tree_refcount(path, root_ref_count, tablet_ref_counts):
            root, tablets = get_chunk_tree(path)
            assert root["ref_counter"] == root_ref_count
            assert [tablet["ref_counter"] for tablet in tablets] == tablet_ref_counts

        verify_chunk_tree_refcount("//tmp/t", 1, [1])

        tx = start_transaction()
        lock("//tmp/t", mode="snapshot", tx=tx)
        verify_chunk_tree_refcount("//tmp/t", 2, [1])

        rows1 = [{"key": i, "value": str(i)} for i in xrange(0, 10, 2)]
        insert_rows("//tmp/t", rows1)
        self.sync_unmount_table("//tmp/t")
        verify_chunk_tree_refcount("//tmp/t", 1, [1])
        assert read_table("//tmp/t") == rows1
        assert read_table("//tmp/t", tx=tx) == []

        abort_transaction(tx)
        verify_chunk_tree_refcount("//tmp/t", 1, [1])

        tx = start_transaction()
        lock("//tmp/t", mode="snapshot", tx=tx)
        verify_chunk_tree_refcount("//tmp/t", 2, [1])

        reshard_table("//tmp/t", [[], [5]])
        verify_chunk_tree_refcount("//tmp/t", 1, [1, 1])

        abort_transaction(tx)
        verify_chunk_tree_refcount("//tmp/t", 1, [1, 1])

        tx = start_transaction()
        lock("//tmp/t", mode="snapshot", tx=tx)
        verify_chunk_tree_refcount("//tmp/t", 2, [1, 1])

        mount_table("//tmp/t", first_tablet_index=0, last_tablet_index=0)
        print "Waiting for tablet 1 to become mounted..."
        self.sync_predicate(lambda: any(x["state"] == "mounted" for x in get("//tmp/t/@tablets")))

        rows2 = [{"key": i, "value": str(i)} for i in xrange(1, 5, 2)]
        insert_rows("//tmp/t", rows2)
        self.sync_unmount_table("//tmp/t")
        verify_chunk_tree_refcount("//tmp/t", 1, [1, 2])
        assert_items_equal(read_table("//tmp/t"), rows1 + rows2)
        assert read_table("//tmp/t", tx=tx) == rows1

        self.sync_mount_table("//tmp/t")
        rows3 = [{"key": i, "value": str(i)} for i in xrange(5, 10, 2)]
        insert_rows("//tmp/t", rows3)
        self.sync_unmount_table("//tmp/t")
        verify_chunk_tree_refcount("//tmp/t", 1, [1, 1])
        assert_items_equal(read_table("//tmp/t"), rows1 + rows2 + rows3)
        assert read_table("//tmp/t", tx=tx) == rows1

        abort_transaction(tx)
        verify_chunk_tree_refcount("//tmp/t", 1, [1, 1])

    def test_write_table(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        with pytest.raises(YtError): write_table("//tmp/t", [{"key": 1, "value": 2}])

    @pytest.mark.skipif('os.environ.get("BUILD_ENABLE_LLVM", None) == "NO"')
    def test_computed_columns(self):
        self.sync_create_cells(1, 1)

        create("table", "//tmp/t1",
            attributes = {
                "schema": [
                    {"name": "key1", "type": "int64", "expression": "key2"},
                    {"name": "key2", "type": "uint64"},
                    {"name": "value", "type": "string"}],
                "key_columns": ["key1", "key2"]
            })
        with pytest.raises(YtError): self.sync_mount_table("//tmp/t1")

        self._create_table_with_computed_column("//tmp/t")
        self.sync_mount_table("//tmp/t")

        insert_rows("//tmp/t", [{"key1": 1, "value": "2"}])
        expected = [{"key1": 1, "key2": 103, "value": "2"}]
        actual = select_rows("* from [//tmp/t]")
        assert actual == expected

        insert_rows("//tmp/t", [{"key1": 2, "value": "2"}])
        expected = [{"key1": 1, "key2": 103, "value": "2"}]
        actual = lookup_rows("//tmp/t", [{"key1" : 1}])
        assert actual == expected
        expected = [{"key1": 2, "key2": 203, "value": "2"}]
        actual = lookup_rows("//tmp/t", [{"key1": 2}])
        assert actual == expected

        delete_rows("//tmp/t", [{"key1": 1}])
        expected = [{"key1": 2, "key2": 203, "value": "2"}]
        actual = select_rows("* from [//tmp/t]")
        assert actual == expected

        with pytest.raises(YtError): insert_rows("//tmp/t", [{"key1": 3, "key2": 3, "value": "3"}])
        with pytest.raises(YtError): lookup_rows("//tmp/t", [{"key1": 2, "key2": 203}])
        with pytest.raises(YtError): delete_rows("//tmp/t", [{"key1": 2, "key2": 203}])

        expected = []
        actual = lookup_rows("//tmp/t", [{"key1": 3}])
        assert actual == expected

        expected = [{"key1": 2, "key2": 203, "value": "2"}]
        actual = select_rows("* from [//tmp/t]")
        assert actual == expected

    @pytest.mark.skipif('os.environ.get("BUILD_ENABLE_LLVM", None) == "NO"')
    def test_computed_hash(self):
        self.sync_create_cells(1, 1)

        self._create_table_with_hash("//tmp/t")
        self.sync_mount_table("//tmp/t")

        row1 = [{"key": 1, "value": "2"}]
        insert_rows("//tmp/t", row1)
        actual = select_rows("key, value from [//tmp/t]")
        assert actual == row1

        row2 = [{"key": 2, "value": "2"}]
        insert_rows("//tmp/t", row2)
        actual = lookup_rows("//tmp/t", [{"key": 1}], column_names=["key", "value"])
        assert actual == row1
        actual = lookup_rows("//tmp/t", [{"key": 2}], column_names=["key", "value"])
        assert actual == row2

        delete_rows("//tmp/t", [{"key": 1}])
        actual = select_rows("key, value from [//tmp/t]")
        assert actual == row2

    @pytest.mark.skipif('os.environ.get("BUILD_ENABLE_LLVM", None) == "NO"')
    def test_computed_column_update_consistency(self):
        self.sync_create_cells(1, 1)

        create("table", "//tmp/t",
            attributes = {
                "schema": [
                    {"name": "key1", "type": "int64", "expression": "key2"},
                    {"name": "key2", "type": "int64"},
                    {"name": "value1", "type": "string"},
                    {"name": "value2", "type": "string"}],
                "key_columns": ["key1", "key2"]
            })
        self.sync_mount_table("//tmp/t")

        insert_rows("//tmp/t", [{"key2": 1, "value1": "2"}])
        expected = [{"key1": 1, "key2": 1, "value1": "2", "value2" : YsonEntity()}]
        actual = lookup_rows("//tmp/t", [{"key2" : 1}])
        assert actual == expected

        insert_rows("//tmp/t", [{"key2": 1, "value2": "3"}], update=True)
        expected = [{"key1": 1, "key2": 1, "value1": "2", "value2": "3"}]
        actual = lookup_rows("//tmp/t", [{"key2" : 1}])
        assert actual == expected

        insert_rows("//tmp/t", [{"key2": 1, "value1": "4"}], update=True)
        expected = [{"key1": 1, "key2": 1, "value1": "4", "value2": "3"}]
        actual = lookup_rows("//tmp/t", [{"key2" : 1}])
        assert actual == expected

    def test_reshard_data(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t1")
        self.sync_mount_table("//tmp/t1")

        def reshard(pivots):
            self.sync_unmount_table("//tmp/t1")
            reshard_table("//tmp/t1", pivots)
            self.sync_mount_table("//tmp/t1")
            #clear_metadata_caches()

        rows = [{"key": i, "value": str(i)} for i in xrange(3)]
        insert_rows("//tmp/t1", rows)
        assert_items_equal(select_rows("* from [//tmp/t1]"), rows)

        reshard([[], [1]])
        assert_items_equal(select_rows("* from [//tmp/t1]"), rows)

        reshard([[], [1], [2]])
        assert_items_equal(select_rows("* from [//tmp/t1]"), rows)

        reshard([[]])
        assert_items_equal(select_rows("* from [//tmp/t1]"), rows)

    def test_no_copy(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t1")
        self.sync_mount_table("//tmp/t1")

        with pytest.raises(YtError): copy("//tmp/t1", "//tmp/t2")

    def test_no_move_mounted(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t1")
        self.sync_mount_table("//tmp/t1")

        with pytest.raises(YtError): move("//tmp/t1", "//tmp/t2")

    def test_move_unmounted(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t1")
        self.sync_mount_table("//tmp/t1")
        self.sync_unmount_table("//tmp/t1")

        table_id1 = get("//tmp/t1/@id")
        tablet_id = get("//tmp/t1/@tablets/0/tablet_id")
        assert get("#" + tablet_id + "/@table_id") == table_id1

        move("//tmp/t1", "//tmp/t2")

        table_id2 = get("//tmp/t2/@id")
        assert get("#" + tablet_id + "/@table_id") == table_id2


    def test_any_value_type(self):
        self.sync_create_cells(1, 1)
        create("table", "//tmp/t1",
            attributes = {
                "schema": [{"name": "key", "type": "int64"}, {"name": "value", "type": "any"}],
                "key_columns": ["key"]
            })
        self.sync_mount_table("//tmp/t1")

        rows = [
            {"key": 11, "value": 100},
            {"key": 12, "value": False},
            {"key": 13, "value": True},
            {"key": 14, "value": 2**63 + 1 },
            {"key": 15, "value": 'stroka'},
            {"key": 16, "value": [1, {"attr": 3}, 4]},
            {"key": 17, "value": {"numbers": [0,1,42]}}]

        insert_rows("//tmp/t1", rows)
        actual = select_rows("* from [//tmp/t1]")
        assert_items_equal(actual, rows)
        actual = lookup_rows("//tmp/t1", [{"key": row["key"]} for row in rows])
        assert_items_equal(actual, rows)

    def test_swap(self):
        self.test_move_unmounted()

        self._create_table("//tmp/t3")
        self.sync_mount_table("//tmp/t3")
        self.sync_unmount_table("//tmp/t3")
        
        reshard_table("//tmp/t3", [[], [100], [200], [300], [400]])
        self.sync_mount_table("//tmp/t3")
        self.sync_unmount_table("//tmp/t3")

        move("//tmp/t3", "//tmp/t1")

        assert self._get_pivot_keys("//tmp/t1") == [[], [100], [200], [300], [400]]

    def _prepare_allowed(self, permission):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")
        create_user("u")
        set("//tmp/t/@inherit_acl", False)
        set("//tmp/t/@acl", [{"permissions": [permission], "action": "allow", "subjects": ["u"]}])

    def _prepare_denied(self, permission):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")
        create_user("u")
        set("//tmp/t/@acl", [{"permissions": [permission], "action": "deny", "subjects": ["u"]}])

    def test_select_allowed(self):
        self._prepare_allowed("read")
        insert_rows("//tmp/t", [{"key": 1, "value": "test"}])
        expected = [{"key": 1, "value": "test"}]
        actual = select_rows("* from [//tmp/t]", user="u")
        assert actual == expected

    def test_select_denied(self):
        self._prepare_denied("read")
        with pytest.raises(YtError): select_rows("* from [//tmp/t]", user="u")

    def test_lookup_allowed(self):
        self._prepare_allowed("read")
        insert_rows("//tmp/t", [{"key": 1, "value": "test"}])
        expected = [{"key": 1, "value": "test"}]
        actual = lookup_rows("//tmp/t", [{"key" : 1}], user="u")
        assert actual == expected

    def test_lookup_denied(self):
        self._prepare_denied("read")
        insert_rows("//tmp/t", [{"key": 1, "value": "test"}])
        with pytest.raises(YtError): lookup_rows("//tmp/t", [{"key" : 1}], user="u")

    def test_insert_allowed(self):
        self._prepare_allowed("write")
        insert_rows("//tmp/t", [{"key": 1, "value": "test"}], user="u")
        expected = [{"key": 1, "value": "test"}]
        actual = lookup_rows("//tmp/t", [{"key" : 1}])
        assert actual == expected

    def test_insert_denied(self):
        self._prepare_denied("write")
        with pytest.raises(YtError): insert_rows("//tmp/t", [{"key": 1, "value": "test"}], user="u")

    def test_delete_allowed(self):
        self._prepare_allowed("write")
        insert_rows("//tmp/t", [{"key": 1, "value": "test"}])
        delete_rows("//tmp/t", [{"key": 1}], user="u")
        expected = []
        actual = lookup_rows("//tmp/t", [{"key" : 1}])
        assert actual == expected

    def test_delete_denied(self):
        self._prepare_denied("write")
        with pytest.raises(YtError): delete_rows("//tmp/t", [{"key": 1}], user="u")

    def test_read_from_chunks(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")

        pivots = [[]] + [[x] for x in range(100, 1000, 100)]
        reshard_table("//tmp/t", pivots)
        assert self._get_pivot_keys("//tmp/t") == pivots

        self.sync_mount_table("//tmp/t")

        rows = [{"key": i, "value": str(i)} for i in xrange(0, 1000, 2)]
        insert_rows("//tmp/t", rows)

        self.sync_unmount_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        actual = lookup_rows("//tmp/t", [{'key': i} for i in xrange(0, 1000)])
        assert_items_equal(actual, rows)

        rows = [{"key": i, "value": str(i)} for i in xrange(1, 1000, 2)]
        insert_rows("//tmp/t", rows)

        self.sync_unmount_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        rows = [{"key": i, "value": str(i)} for i in xrange(0, 1000)]
        actual = lookup_rows("//tmp/t", [{'key': i} for i in xrange(0, 1000)])
        assert_items_equal(actual, rows)

        sleep(1)
        for tablet in xrange(10):
            path = "//tmp/t/@tablets/%s/performance_counters" % tablet
            assert get(path + "/static_chunk_row_lookup_count") == 200
            #assert get(path + "/static_chunk_row_lookup_false_positive_count") < 4
            #assert get(path + "/static_chunk_row_lookup_true_negative_count") > 90

    def test_store_rotation(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")

        set("//tmp/t/@max_memory_store_key_count", 10)
        self.sync_mount_table("//tmp/t")

        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        address = self._get_tablet_leader_address(tablet_id)

        rows = [{"key": i, "value": str(i)} for i in xrange(10)]
        insert_rows("//tmp/t", rows)

        sleep(3.0)

        tablet_data = self._find_tablet_orchid(address, tablet_id)
        assert len(tablet_data["eden"]["stores"]) == 1
        assert len(tablet_data["partitions"]) == 1
        assert len(tablet_data["partitions"][0]["stores"]) == 1

    def _test_in_memory(self, mode):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")

        set("//tmp/t/@in_memory_mode", mode)
        set("//tmp/t/@max_memory_store_key_count", 10)
        self.sync_mount_table("//tmp/t")

        tablet_id = get("//tmp/t/@tablets/0/tablet_id")
        address = self._get_tablet_leader_address(tablet_id)

        def _check_preload_state(state):
            tablet_data = self._find_tablet_orchid(address, tablet_id)
            assert len(tablet_data["eden"]["stores"]) == 1
            assert len(tablet_data["partitions"]) == 1
            assert len(tablet_data["partitions"][0]["stores"]) >= 1
            assert all(s["preload_state"] == state for _, s in tablet_data["partitions"][0]["stores"].iteritems())
            actual_preload_completed = get("//tmp/t/@tablets/0/statistics/store_preload_completed_count")
            if state == "complete":
                assert actual_preload_completed >= 1
            else:
                assert actual_preload_completed == 0
            assert get("//tmp/t/@tablets/0/statistics/store_preload_pending_count") == 0
            assert get("//tmp/t/@tablets/0/statistics/store_preload_failed_count") == 0

        rows = [{"key": i, "value": str(i)} for i in xrange(10)]
        insert_rows("//tmp/t", rows)

        sleep(3.0)

        _check_preload_state("complete")

        set("//tmp/t/@in_memory_mode", "none")
        remount_table("//tmp/t")

        sleep(3.0)

        _check_preload_state("disabled")

        set("//tmp/t/@in_memory_mode", mode)
        remount_table("//tmp/t")

        sleep(3.0)

        _check_preload_state("complete")

    def test_in_memory_compressed(self):
        self._test_in_memory("compressed")

    def test_in_memory_uncompressed(self):
        self._test_in_memory("uncompressed")

    def test_update_key_columns_fail1(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")
        with pytest.raises(YtError): set("//tmp/t/@key_columns", ["key", "key2"])

    def test_update_key_columns_fail2(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        with pytest.raises(YtError): set("//tmp/t/@key_columns", ["key2", "key3"])

    def test_update_key_columns_fail3(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        with pytest.raises(YtError): set("//tmp/t/@key_columns", [])

    def test_update_schema_fails(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")
        self.sync_unmount_table("//tmp/t")
        with pytest.raises(YtError): set("//tmp/t/@schema", [
            {"name": "key1", "type": "int64"},
            {"name": "value", "type": "string"}])
        with pytest.raises(YtError): set("//tmp/t/@schema", [
            {"name": "key", "type": "uint64"},
            {"name": "value", "type": "string"}])
        with pytest.raises(YtError): set("//tmp/t/@schema", [
            {"name": "key", "type": "int64"},
            {"name": "value1", "type": "string"}])

        self._create_table_with_computed_column("//tmp/t1")
        self.sync_mount_table("//tmp/t1")
        self.sync_unmount_table("//tmp/t1")
        with pytest.raises(YtError): set("//tmp/t1/@schema", [
            {"name": "key1", "type": "int64"},
            {"name": "key2", "type": "int64"},
            {"name": "value", "type": "string"}])
        with pytest.raises(YtError): set("//tmp/t1/@schema", [
            {"name": "key1", "type": "int64", "expression": "key2 * 100 + 3"},
            {"name": "key2", "type": "int64"},
            {"name": "value", "type": "string"}])
        with pytest.raises(YtError): set("//tmp/t1/@schema", [
            {"name": "key1", "type": "int64"},
            {"name": "key2", "type": "int64", "expression": "key1 * 100"},
            {"name": "value", "type": "string"}])
        with pytest.raises(YtError): set("//tmp/t1/@schema", [
            {"name": "key1", "type": "int64"},
            {"name": "key2", "type": "int64", "expression": "key1 * 100 + 3"},
            {"name": "key3", "type": "int64", "expression": "key1 * 100 + 3"},
            {"name": "value", "type": "string"}])

    def test_update_key_columns_success(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        
        self.sync_mount_table("//tmp/t")
        rows1 = [{"key": i, "value": str(i)} for i in xrange(100)]
        insert_rows("//tmp/t", rows1)
        self.sync_unmount_table("//tmp/t")

        set("//tmp/t/@key_columns", ["key", "key2"])
        set("//tmp/t/@schema/after:0", {"name": "key2", "type": "int64"})
        self.sync_mount_table("//tmp/t")

        rows2 = [{"key": i, "key2": 0, "value": str(i)} for i in xrange(100)]
        insert_rows("//tmp/t", rows2)

        assert lookup_rows("//tmp/t", [{"key" : 77}]) == [{"key": 77, "key2": YsonEntity(), "value": "77"}]
        assert lookup_rows("//tmp/t", [{"key" : 77, "key2": 1}]) == []
        assert lookup_rows("//tmp/t", [{"key" : 77, "key2": 0}]) == [{"key": 77, "key2": 0, "value": "77"}]
        assert select_rows("sum(1) as s from [//tmp/t] where is_null(key2) group by 0") == [{"s": 100}]

    def test_atomicity_mode_should_match(self):
        def do(a1, a2):
            self.sync_create_cells(1, 1)
            self._create_table("//tmp/t", atomicity=a1)
            self.sync_mount_table("//tmp/t")
            rows = [{"key": i, "value": str(i)} for i in xrange(100)]
            with pytest.raises(YtError): insert_rows("//tmp/t", rows, atomicity=a2)
            remove("//tmp/t")

            clear_metadata_caches()

        do("full", "none")
        do("none", "full")

    def _test_snapshots(self, atomicity):
        self.sync_create_cells(1, 1)
        cell_id = ls("//sys/tablet_cells")[0]

        self._create_table("//tmp/t", atomicity=atomicity)
        self.sync_mount_table("//tmp/t")
        
        rows = [{"key": i, "value": str(i)} for i in xrange(100)]
        insert_rows("//tmp/t", rows, atomicity=atomicity)

        build_snapshot(cell_id=cell_id)

        snapshots = ls("//sys/tablet_cells/" + cell_id + "/snapshots")
        assert len(snapshots) == 1

        self.Env.kill_service("node")
        # Wait for make sure all leases have expired
        time.sleep(3.0)
        self.Env.start_nodes("node")

        self.wait_for_cells()

        # Wait for make all tablets are up
        time.sleep(3.0)

        keys = [{"key": i} for i in xrange(100)]
        actual = lookup_rows("//tmp/t", keys);
        assert_items_equal(actual, rows);

    def test_atomic_snapshots(self):
        self._test_snapshots("full")

    def test_nonatomic_snapshots(self):
        self._test_snapshots("none")

    def test_stress_tablet_readers(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        self.sync_mount_table("//tmp/t")

        values = dict()

        def verify():
            expected = [{"key": key, "value": values[key]} for key in values.keys()]
            actual = select_rows("* from [//tmp/t]")
            assert_items_equal(actual, expected)

            keys = list(values.keys())[::2]
            for i in xrange(len(keys)):
                if i % 3 == 0:
                    j = (i * 34567) % len(keys)
                    keys[i], keys[j] = keys[j], keys[i]

            expected = [{"key": key, "value": values[key]} for key in keys]

            if len(keys) > 0:
                actual = select_rows("* from [//tmp/t] where key in (%s)" % ",".join([str(key) for key in keys]))
                assert_items_equal(actual, expected)

            actual = lookup_rows("//tmp/t", [{"key": key} for key in keys])
            assert actual == expected

        verify()

        rounds = 10
        items = 100

        for wave in xrange(1, rounds):
            rows = [{"key": i, "value": str(i + wave * 100)} for i in xrange(0, items, wave)]
            for row in rows:
                values[row["key"]] = row["value"]
            print "Write rows ", rows
            insert_rows("//tmp/t", rows)

            verify()

            self.sync_unmount_table("//tmp/t")
            pivots = ([[]] + [[x] for x in xrange(0, items, items / wave)]) if wave % 2 == 0 else [[]]
            reshard_table("//tmp/t", pivots)
            self.sync_mount_table("//tmp/t")

            verify()

            keys = sorted(list(values.keys()))[::(wave * 12345) % items]
            print "Delete keys ", keys
            rows = [{"key": key} for key in keys]
            delete_rows("//tmp/t", rows)
            for key in keys:
                values.pop(key)

            verify()

    def test_read_only_mode(self):
        self.sync_create_cells(1, 1)
        self._create_table("//tmp/t")
        set("//tmp/t/@read_only", True)
        self.sync_mount_table("//tmp/t")

        rows = [{"key": i, "value": str(i)} for i in xrange(1)]

        with pytest.raises(YtError): insert_rows("//tmp/t", rows)

        remove("//tmp/t/@read_only")
        remount_table("//tmp/t")

        insert_rows("//tmp/t", rows)

        set("//tmp/t/@read_only", True)
        remount_table("//tmp/t")

