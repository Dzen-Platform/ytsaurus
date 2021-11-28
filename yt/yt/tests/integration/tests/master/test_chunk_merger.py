from yt_env_setup import YTEnvSetup

from yt_commands import (
    authors, wait, create, ls, get, set, copy, remove,
    exists, concatenate,
    create_account, create_user, make_ace, insert_rows,
    alter_table, read_table, write_table, map, merge,
    sync_create_cells, sync_mount_table,
    start_transaction, abort_transaction, commit_transaction,
    sync_unmount_table, create_dynamic_table)

from yt_helpers import get_all_master_counters

from yt_type_helpers import make_schema

from yt.common import YtError
import yt.yson as yson

import pytest

from time import sleep

#################################################################


def _schematize_row(row, schema):
    result = {}
    for column in schema:
        name = column["name"]
        result[name] = row.get(name, yson.YsonEntity())
    return result


def _schematize_rows(rows, schema):
    return [_schematize_row(row, schema) for row in rows]


class TestChunkMerger(YTEnvSetup):
    NUM_TEST_PARTITIONS = 12

    NUM_MASTERS = 5
    NUM_NODES = 3
    NUM_SCHEDULERS = 1
    USE_DYNAMIC_TABLES = True
    ENABLE_BULK_INSERT = True
    ENABLE_RPC_PROXY = True
    DRIVER_BACKEND = "rpc"

    DELTA_MASTER_CONFIG = {
        "chunk_manager": {
            "allow_multiple_erasure_parts_per_node": True,
        }
    }

    DELTA_DYNAMIC_MASTER_CONFIG = {
        "chunk_manager": {
            "chunk_merger": {
                "max_chunk_count": 5,
                "create_chunks_period": 100,
                "schedule_period": 100,
                "session_finalization_period": 100,
            }
        }
    }

    def _get_chunk_merger_txs(self):
        txs = []
        for tx in ls("//sys/transactions", attributes=["title"]):
            title = tx.attributes.get("title", "")
            if "Chunk merger" in title:
                txs.append(tx)
        return txs

    def _abort_chunk_merger_txs(self):
        txs = self._get_chunk_merger_txs()
        for tx in txs:
            abort_transaction(tx)

    def _wait_for_merge(self, table_path, merge_mode, account):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        assert get("{}/@resource_usage/chunk_count".format(table_path)) > 1
        rows = read_table(table_path)

        set("{}/@chunk_merger_mode".format(table_path), merge_mode)
        set("//sys/accounts/{}/@merge_job_rate_limit".format(account), 10)
        set("//sys/accounts/{}/@chunk_merger_node_traversal_concurrency".format(account), 1)
        wait(lambda: get("{}/@resource_usage/chunk_count".format(table_path)) == 1)
        assert read_table(table_path) == rows

    @authors("aleksandra-zh")
    def test_merge_attributes(self):
        create("table", "//tmp/t")

        assert get("//tmp/t/@chunk_merger_mode") == "none"
        set("//tmp/t/@chunk_merger_mode", "deep")
        assert get("//tmp/t/@chunk_merger_mode") == "deep"
        set("//tmp/t/@chunk_merger_mode", "shallow")
        assert get("//tmp/t/@chunk_merger_mode") == "shallow"
        set("//tmp/t/@chunk_merger_mode", "auto")
        assert get("//tmp/t/@chunk_merger_mode") == "auto"

        with pytest.raises(YtError):
            set("//tmp/t/@chunk_merger_mode", "sdjkfhdskj")

        create_account("a")

        assert get("//sys/accounts/a/@merge_job_rate_limit") == 0
        set("//sys/accounts/a/@merge_job_rate_limit", 7)
        with pytest.raises(YtError):
            set("//sys/accounts/a/@merge_job_rate_limit", -1)
        assert get("//sys/accounts/a/@merge_job_rate_limit") == 7

        assert get("//sys/accounts/a/@chunk_merger_node_traversal_concurrency") == 0
        set("//sys/accounts/a/@chunk_merger_node_traversal_concurrency", 12)
        with pytest.raises(YtError):
            set("//sys/accounts/a/@chunk_merger_node_traversal_concurrency", -1)
        assert get("//sys/accounts/a/@chunk_merger_node_traversal_concurrency") == 12

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_merge1(self, merge_mode):
        create("table", "//tmp/t")
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})
        write_table("<append=true>//tmp/t", {"a": "e"})

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@data_weight".format(chunk_ids[0])) > 0

    @authors("aleksandra-zh")
    def test_merge2(self):
        create("table", "//tmp/t")
        write_table("//tmp/t", [
            {"a": 10},
            {"b": 50}
        ])
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        self._wait_for_merge("//tmp/t", "deep", "tmp")

    @authors("aleksandra-zh")
    def test_auto_merge1(self):
        create("table", "//tmp/t", attributes={"compression_codec": "lz4"})
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        set("//tmp/t/@compression_codec", "zstd_17")
        write_table("<append=true>//tmp/t", {"q": "e"})

        set("//sys/@config/chunk_manager/chunk_merger/enable", True)
        set("//tmp/t/@chunk_merger_mode", "auto")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)

    @authors("aleksandra-zh")
    def test_auto_merge2(self):
        create("table", "//tmp/t", attributes={"compression_codec": "lz4"})
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        set("//tmp/t/@compression_codec", "zstd_17")
        write_table("<append=true>//tmp/t", {"q": "e"})

        set("//sys/@config/chunk_manager/chunk_merger/enable", True)
        set("//sys/@config/chunk_manager/chunk_merger/min_shallow_merge_chunk_count", 3)
        set("//tmp/t/@chunk_merger_mode", "auto")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 2)

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_merge_remove(self, merge_mode):
        create("table", "//tmp/t")
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        set("//sys/@config/chunk_manager/chunk_merger/enable", True)
        set("//tmp/t/@chunk_merger_mode", merge_mode)
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@is_being_merged") or get("//tmp/t/@resource_usage/chunk_count") == 1)

        for i in range(10):
            write_table("<append=true>//tmp/t", {"a": "b"})

        wait(lambda: not get("//tmp/t/@is_being_merged"))
        remove("//tmp/t")

        wait(lambda: get("//sys/chunk_lists/@count") == len(get("//sys/chunk_lists")))

    @authors("aleksandra-zh")
    def test_merge_does_not_conflict_with_tx_append(self):
        create("table", "//tmp/t")
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        tx = start_transaction()
        write_table("<append=true>//tmp/t", {"d": "e"}, tx=tx)
        rows = read_table("//tmp/t", tx=tx)

        self._wait_for_merge("//tmp/t", "deep", "tmp")
        commit_transaction(tx)

        assert get("//tmp/t/@resource_usage/chunk_count") == 2
        read_table("//tmp/t") == rows

    @authors("aleksandra-zh")
    def test_merge_does_not_conflict_with_tx_overwrite(self):
        create("table", "//tmp/t")
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        tx = start_transaction()
        write_table("//tmp/t", {"d": "e"}, tx=tx)
        rows = read_table("//tmp/t", tx=tx)

        self._wait_for_merge("//tmp/t", "deep", "tmp")
        commit_transaction(tx)

        assert get("//tmp/t/@resource_usage/chunk_count") == 1
        read_table("//tmp/t") == rows

    @authors("aleksandra-zh")
    def test_is_being_merged(self):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        create("table", "//tmp/t")

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        assert get("//tmp/t/@resource_usage/chunk_count") > 1
        rows = read_table("//tmp/t")

        set("//tmp/t/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@is_being_merged"))

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)
        assert read_table("//tmp/t") == rows
        wait(lambda: not get("//tmp/t/@is_being_merged"))

    @authors("aleksandra-zh", "gritukan")
    def test_abort_merge_tx(self):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        create("table", "//tmp/t")

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        rows = read_table("//tmp/t")

        set("//tmp/t/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        for _ in range(10):
            wait(lambda: self._get_chunk_merger_txs() > 0)
            self._abort_chunk_merger_txs()

        rows = read_table("//tmp/t")
        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)
        assert read_table("//tmp/t") == rows

    @authors("aleksandra-zh")
    def test_merge_job_accounting1(self):
        create_account("a")
        create("table", "//tmp/t1", attributes={"account": "a"})

        write_table("<append=true>//tmp/t1", {"a": "b"})
        write_table("<append=true>//tmp/t1", {"b": "c"})
        write_table("<append=true>//tmp/t1", {"c": "d"})

        create_account("b")
        copy("//tmp/t1", "//tmp/t2")
        set("//tmp/t2/@account", "b")

        self._wait_for_merge("//tmp/t2", "deep", "b")

        wait(lambda: get("//sys/accounts/b/@resource_usage/chunk_count") == 1)

        self._abort_chunk_merger_txs()

    @authors("aleksandra-zh")
    def test_merge_job_accounting2(self):
        create_account("a")
        create("table", "//tmp/t1", attributes={"account": "a"})

        write_table("<append=true>//tmp/t1", {"a": "b"})
        write_table("<append=true>//tmp/t1", {"b": "c"})
        write_table("<append=true>//tmp/t1", {"c": "d"})

        create_account("b")
        copy("//tmp/t1", "//tmp/t2")
        set("//tmp/t2/@account", "b")

        set("//tmp/t1/@chunk_merger_mode", "deep")
        self._wait_for_merge("//tmp/t2", "deep", "b")

        assert get("//tmp/t1/@resource_usage/chunk_count") > 1

        set("//sys/accounts/a/@merge_job_rate_limit", 10)
        set("//sys/accounts/a/@chunk_merger_node_traversal_concurrency", 1)
        write_table("<append=true>//tmp/t1", {"c": "d"})

        wait(lambda: get("//tmp/t1/@resource_usage/chunk_count") == 1)

        self._abort_chunk_merger_txs()

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_copy_merge(self, merge_mode):
        create("table", "//tmp/t")
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})

        copy("//tmp/t", "//tmp/t1")
        rows = read_table("//tmp/t")

        self._wait_for_merge("//tmp/t1", merge_mode, "tmp")

        assert get("//tmp/t/@resource_usage/chunk_count") > 1
        assert read_table("//tmp/t") == rows

    @authors("aleksandra-zh")
    def test_schedule_again(self):
        create("table", "//tmp/t")
        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        self._wait_for_merge("//tmp/t", "deep", "tmp")

        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        rows = read_table("//tmp/t")
        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)
        assert read_table("//tmp/t") == rows

    @authors("cookiedoth")
    @pytest.mark.parametrize("enable_erasure", [False, True])
    def test_multiple_merge(self, enable_erasure):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        if enable_erasure:
            create("table", "//tmp/t", attributes={"erasure_codec": "lrc_12_2_2"})
        else:
            create("table", "//tmp/t")

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        set("//tmp/t/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        for i in range(3):
            write_table("<append=true>//tmp/t", {str(2 * i): str(2 * i)})
            wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)

    @authors("aleksandra-zh")
    def test_merge_does_not_overwrite_data(self):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        create("table", "//tmp/t")

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        set("//tmp/t/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@is_being_merged"))

        write_table("//tmp/t", {"q": "r"})
        write_table("<append=true>//tmp/t", {"w": "t"})
        write_table("<append=true>//tmp/t", {"e": "y"})

        rows = read_table("//tmp/t")

        assert get("//tmp/t/@resource_usage/chunk_count") > 1
        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)

        assert read_table("//tmp/t") == rows

    @authors("aleksandra-zh")
    def test_remove(self):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        create("table", "//tmp/t")

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        assert get("//tmp/t/@resource_usage/chunk_count") > 1

        set("//tmp/t/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@is_being_merged"))

        remove("//tmp/t")

        # Just hope nothing crashes.
        sleep(5)

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_schema(self, merge_mode):
        create(
            "table",
            "//tmp/t",
            attributes={
                "schema": make_schema(
                    [
                        {"name": "key", "type": "int64"},
                        {"name": "value", "type": "string"},
                    ]
                ),
            },
        )

        write_table("<append=true>//tmp/t", {"key": 1, "value": "a"})
        write_table("<append=true>//tmp/t", {"key": 2, "value": "b"})
        write_table("<append=true>//tmp/t", {"key": 3, "value": "c"})

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_sorted(self, merge_mode):
        create(
            "table",
            "//tmp/t",
            attributes={
                "schema": make_schema(
                    [
                        {"name": "key", "type": "int64", "sort_order": "ascending"},
                        {"name": "value", "type": "string"},
                    ]
                ),
            },
        )

        write_table("<append=true>//tmp/t", {"key": 1, "value": "a"})
        write_table("<append=true>//tmp/t", {"key": 2, "value": "b"})
        write_table("<append=true>//tmp/t", {"key": 3, "value": "c"})

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

    @authors("aleksandra-zh")
    def test_merge_merge(self):
        create("table", "//tmp/t1")
        write_table("<append=true>//tmp/t1", {"a": "b"})
        write_table("<append=true>//tmp/t1", {"b": "c"})

        create("table", "//tmp/t2")
        write_table("<append=true>//tmp/t2", {"key": 1, "value": "a"})
        write_table("<append=true>//tmp/t2", {"key": 2, "value": "b"})

        create("table", "//tmp/t")
        merge(mode="unordered", in_=["//tmp/t1", "//tmp/t2"], out="//tmp/t")

        self._wait_for_merge("//tmp/t", "deep", "tmp")

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_merge_chunks_exceed_max_chunk_to_merge_limit(self, merge_mode):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        create("table", "//tmp/t")
        for i in range(10):
            write_table("<append=true>//tmp/t", {"a": "b"})

        assert get("//tmp/t/@resource_usage/chunk_count") == 10
        rows = read_table("//tmp/t")

        set("//tmp/t/@chunk_merger_mode", merge_mode)
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") <= 2)
        assert read_table("//tmp/t") == rows

        # Initiate another merge.
        write_table("<append=true>//tmp/t", {"a": "b"})
        rows = read_table("//tmp/t")

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)
        assert read_table("//tmp/t") == rows

    @authors("aleksandra-zh")
    def test_merge_job_rate_limit_permission(self):
        create_account("a")
        create_user("u")
        acl = [
            make_ace("allow", "u", ["use", "modify_children"], "object_only"),
            make_ace("allow", "u", ["write", "remove", "administer"], "descendants_only"),
        ]
        set("//sys/account_tree/a/@acl", acl)

        create_account("b", "a", authenticated_user="u")

        with pytest.raises(YtError):
            set("//sys/accounts/a/@merge_job_rate_limit", 10, authenticated_user="u")
            set("//sys/accounts/a/@chunk_merger_node_traversal_concurrency", 10, authenticated_user="u")

        with pytest.raises(YtError):
            set("//sys/accounts/b/@merge_job_rate_limit", 10, authenticated_user="u")
            set("//sys/accounts/b/@chunk_merger_node_traversal_concurrency", 10, authenticated_user="u")

        create("table", "//tmp/t")
        with pytest.raises(YtError):
            set("//tmp/t/@chunk_merger_mode", "shallow", authenticated_user="u")
        with pytest.raises(YtError):
            set("//tmp/t/@chunk_merger_mode", "deep", authenticated_user="u")

    @authors("aleksandra-zh")
    def test_do_not_crash_on_dynamic_table(self):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        sync_create_cells(1)
        create("table", "//tmp/t1")

        schema = [
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "string"},
        ]

        create_dynamic_table("//tmp/t2", schema=schema)
        sync_mount_table("//tmp/t2")

        insert_rows("//tmp/t2", [{"key": 1, "value": "1"}])
        insert_rows("//tmp/t2", [{"key": 2, "value": "1"}])

        sync_unmount_table("//tmp/t2")
        sync_mount_table("//tmp/t2")

        write_table("<append=true>//tmp/t1", {"key": 3, "value": "1"})

        map(in_="//tmp/t1", out="<append=true>//tmp/t2", command="cat")

        set("//tmp/t2/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        # Just do not crash, please.
        sleep(10)

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_compression_codec(self, merge_mode):
        codec = "lz4"
        create("table", "//tmp/t", attributes={"compression_codec": codec})

        write_table("<append=true>//tmp/t", {"a": "b", "b": "c"})
        write_table("<append=true>//tmp/t", {"a": "d", "b": "e"})

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@compression_codec".format(chunk_ids[0])) == codec

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@compression_codec".format(chunk_ids[0])) == codec

    @authors("aleksandra-zh")
    def test_change_compression_codec(self):
        codec1 = "lz4"
        codec2 = "zstd_17"
        create("table", "//tmp/t", attributes={"compression_codec": codec1})

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"b": "c"})
        write_table("<append=true>//tmp/t", {"c": "d"})

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@compression_codec".format(chunk_ids[0])) == codec1

        set("//tmp/t/@compression_codec", codec2)

        self._wait_for_merge("//tmp/t", "deep", "tmp")

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@compression_codec".format(chunk_ids[0])) == codec2

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_erasure1(self, merge_mode):
        codec = "lrc_12_2_2"
        create("table", "//tmp/t", attributes={"erasure_codec": codec})

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@erasure_codec".format(chunk_ids[0])) == codec

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_erasure2(self, merge_mode):
        codec = "lrc_12_2_2"
        none_codec = "none"
        create("table", "//tmp/t", attributes={"erasure_codec": codec})

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        set("//tmp/t/@erasure_codec", none_codec)

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert not exists("#{0}/@erasure_codec".format(chunk_ids[0]))

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_erasure3(self, merge_mode):
        codec = "lrc_12_2_2"
        none_codec = "none"
        create("table", "//tmp/t")

        write_table("<append=true>//tmp/t", {"a": "b"})
        set("//tmp/t/@erasure_codec", codec)
        write_table("<append=true>//tmp/t", {"a": "c"})
        set("//tmp/t/@erasure_codec", none_codec)
        write_table("<append=true>//tmp/t", {"a": "d"})

        set("//tmp/t/@erasure_codec", codec)

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@erasure_codec".format(chunk_ids[0])) == codec

    @authors("aleksandra-zh")
    @pytest.mark.parametrize(
        "optimize_for, merge_mode",
        [("scan", "deep"), ("scan", "shallow"), ("lookup", "deep"), ("lookup", "shallow")]
    )
    def test_optimize_for(self, optimize_for, merge_mode):
        create("table", "//tmp/t", attributes={"optimize_for": optimize_for})

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        chunk_format = "table_schemaless_horizontal" if optimize_for == "lookup" else "table_unversioned_columnar"
        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@chunk_format".format(chunk_ids[0])) == chunk_format

    @authors("aleksandra-zh")
    @pytest.mark.parametrize(
        "optimize_for, merge_mode",
        [("scan", "deep"), ("scan", "shallow"), ("lookup", "deep"), ("lookup", "shallow")]
    )
    def test_read_rows(self, optimize_for, merge_mode):
        create("table", "//tmp/t", attributes={"optimize_for": optimize_for})
        set("//tmp/t/@chunk_writer", {"block_size": 1})

        write_table("<append=true>//tmp/t", [{"a": "z"}, {"b": "a"}, {"c": "q"}])
        write_table("<append=true>//tmp/t", [{"a": "x"}, {"b": "s"}, {"c": "w"}])
        write_table("<append=true>//tmp/t", [{"a": "c"}, {"b": "d"}, {"c": "e"}])

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        assert read_table("//tmp/t[#-1:#2]") == [{"a": "z"}, {"b": "a"}]
        assert read_table("//tmp/t[#2:#4]") == [{"c": "q"}, {"a": "x"}]
        assert read_table("//tmp/t[#5:#8]") == [{"c": "w"}, {"a": "c"}, {"b": "d"}]
        assert read_table("//tmp/t[#4:#5]") == [{"b": "s"}]
        assert read_table("//tmp/t[#7:#11]") == [{"b": "d"}, {"c": "e"}]

    @authors("aleksandra-zh")
    @pytest.mark.parametrize(
        "optimize_for, merge_mode",
        [("scan", "deep"), ("scan", "shallow"), ("lookup", "deep"), ("lookup", "shallow")]
    )
    def test_row_key_selector(self, optimize_for, merge_mode):
        create("table", "//tmp/t", attributes={"optimize_for": optimize_for})
        set("//tmp/t/@chunk_writer", {"block_size": 1})

        v1 = {"s": "a", "i": 0, "d": 15.5}
        v2 = {"s": "a", "i": 10, "d": 15.2}
        v3 = {"s": "b", "i": 5, "d": 20.0}
        write_table("<append=true>//tmp/t", [v1, v2], sorted_by=["s", "i", "d"])

        v4 = {"s": "b", "i": 20, "d": 20.0}
        v5 = {"s": "c", "i": -100, "d": 10.0}
        write_table("<append=true>//tmp/t", [v3, v4, v5], sorted_by=["s", "i", "d"])

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        assert read_table("//tmp/t[a : a]") == []
        assert read_table("//tmp/t[(a, 1) : (a, 10)]") == []
        assert read_table("//tmp/t[b : a]") == []
        assert read_table("//tmp/t[(c, 0) : (a, 10)]") == []
        assert read_table("//tmp/t[(a, 10, 1e7) : (b, )]") == []

        assert read_table("//tmp/t[c:]") == [v5]
        assert read_table("//tmp/t[:(a, 10)]") == [v1]
        assert read_table("//tmp/t[:(a, 10),:(a, 10)]") == [v1, v1]
        assert read_table("//tmp/t[:(a, 11)]") == [v1, v2]
        assert read_table("//tmp/t[:]") == [v1, v2, v3, v4, v5]
        assert read_table("//tmp/t[a : b , b : c]") == [v1, v2, v3, v4]
        assert read_table("//tmp/t[a]") == [v1, v2]
        assert read_table("//tmp/t[(a,10)]") == [v2]
        assert read_table("//tmp/t[a,c]") == [v1, v2, v5]

        assert read_table("//tmp/t{s, d}[aa: (b, 10)]") == [{"s": "b", "d": 20.0}]
        assert read_table("//tmp/t[#0:c]") == [v1, v2, v3, v4]

    @authors("aleksandra-zh")
    @pytest.mark.parametrize(
        "optimize_for, merge_mode",
        [("scan", "deep"), ("scan", "shallow"), ("lookup", "deep"), ("lookup", "shallow")]
    )
    def test_column_selector(self, optimize_for, merge_mode):
        create("table", "//tmp/t", attributes={"optimize_for": optimize_for})

        write_table("<append=true>//tmp/t", {"a": 1, "aa": 2, "b": 3, "bb": 4, "c": 5})
        write_table("<append=true>//tmp/t", {"a": 11, "aa": 22, "b": 33, "bb": 44, "c": 55})
        write_table("<append=true>//tmp/t", {"a": 111, "aa": 222, "b": 333, "bb": 444, "c": 555})

        copy("//tmp/t", "//tmp/t1")
        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        assert get("//tmp/t1/@resource_usage/chunk_count") > 1

        assert read_table("//tmp/t{}") == read_table("//tmp/t1{}")

        assert read_table("//tmp/t{a}") == read_table("//tmp/t1{a}")
        assert read_table("//tmp/t{a, }") == read_table("//tmp/t1{a, }")
        assert read_table("//tmp/t{a, a}") == read_table("//tmp/t1{a, a}")
        assert read_table("//tmp/t{c, b}") == read_table("//tmp/t1{c, b}")
        assert read_table("//tmp/t{zzzzz}") == read_table("//tmp/t1{zzzzz}")

        assert read_table("//tmp/t{a}") == read_table("//tmp/t1{a}")
        assert read_table("//tmp/t{a, }") == read_table("//tmp/t1{a, }")
        assert read_table("//tmp/t{a, a}") == read_table("//tmp/t1{a, a}")
        assert read_table("//tmp/t{c, b}") == read_table("//tmp/t1{c, b}")
        assert read_table("//tmp/t{zzzzz}") == read_table("//tmp/t1{zzzzz}")

    @authors("babenko", "h0pless")
    @pytest.mark.parametrize(
        "optimize_for, merge_mode",
        [("scan", "auto"), ("lookup" , "auto"), ("scan", "deep"), ("lookup" , "deep")]
    )
    def test_nonstrict_schema(self, optimize_for, merge_mode):
        schema = make_schema(
            [
                {"name": "a", "type": "string"},
                {"name": "b", "type": "string"},
                {"name": "c", "type": "int64"}
            ],
            strict=False
        )
        create("table", "//tmp/t", attributes={"optimize_for": optimize_for, "schema": schema})

        rows1 = [{"a": "a" + str(i), "b": "b" + str(i), "c": i, "x": "x" + str(i)} for i in xrange(0, 10)]
        write_table("<append=true>//tmp/t", rows1)
        rows2 = [{"a": "a" + str(i), "b": "b" + str(i), "c": i, "y": "y" + str(i)} for i in xrange(10, 20)]
        write_table("<append=true>//tmp/t", rows2)
        rows3 = [{"a": "a" + str(i), "b": "b" + str(i), "c": i, "z": "z" + str(i)} for i in xrange(20, 30)]
        write_table("<append=true>//tmp/t", rows3)
        assert read_table("//tmp/t") == rows1 + rows2 + rows3

        fallback_counter = get_all_master_counters("chunk_server/chunk_merger_auto_merge_fallback_count")
        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        if merge_mode == "auto":
            wait(lambda: sum(counter.get_delta() for counter in fallback_counter) > 0)

        chunk_format = "table_schemaless_horizontal" if optimize_for == "lookup" else "table_unversioned_columnar"
        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@chunk_format".format(chunk_ids[0])) == chunk_format

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("optimize_for", ["scan", "lookup"])
    def test_alter_schema(self, optimize_for):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)
        set("//sys/@config/chunk_manager/chunk_merger/max_chunk_count", 10)

        schema1 = make_schema(
            [
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"},
            ]
        )
        schema2 = make_schema(
            [
                {"name": "key", "type": "int64"},
                {"name": "another_value", "type": "string"},
                {"name": "value", "type": "string"},
            ]
        )

        create("table", "//tmp/t", attributes={"schema": schema1, "optimize_for": optimize_for})
        write_table("<append=true>//tmp/t", {"key": 1, "value": "a"})
        alter_table("//tmp/t", schema=schema2)
        write_table("<append=true>//tmp/t", {"key": 2, "another_value": "z", "value": "b"})
        write_table("<append=true>//tmp/t", {"key": 3, "value": "c"})
        create("table", "//tmp/t1", attributes={"schema": schema2, "optimize_for": optimize_for})
        write_table("<append=true>//tmp/t1", {"key": 4, "another_value": "x", "value": "e"})
        concatenate(["//tmp/t1", "//tmp/t", "//tmp/t1", "//tmp/t"], "//tmp/t")

        rows = read_table("//tmp/t")

        set("//tmp/t/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)
        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)
        merged_rows = read_table("//tmp/t")

        assert _schematize_rows(rows, schema2) == _schematize_rows(merged_rows, schema2)

    @authors("aleksandra-zh")
    def test_alter_schema_shallow(self):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)
        set("//sys/@config/chunk_manager/chunk_merger/max_chunk_count", 10)

        schema1 = make_schema(
            [
                {"name": "key", "type": "int64"},
                {"name": "value", "type": "string"},
            ]
        )
        schema2 = make_schema(
            [
                {"name": "key", "type": "int64"},
                {"name": "another_value", "type": "string"},
                {"name": "value", "type": "string"},
            ]
        )

        create("table", "//tmp/t", attributes={"schema": schema1})
        write_table("<append=true>//tmp/t", {"key": 1, "value": "a"})
        alter_table("//tmp/t", schema=schema2)
        write_table("<append=true>//tmp/t", {"key": 2, "another_value": "z", "value": "b"})
        write_table("<append=true>//tmp/t", {"key": 3, "value": "c"})
        create("table", "//tmp/t1", attributes={"schema": schema2})
        write_table("<append=true>//tmp/t1", {"key": 4, "another_value": "x", "value": "e"})
        concatenate(["//tmp/t1", "//tmp/t", "//tmp/t1", "//tmp/t"], "//tmp/t")

        rows = read_table("//tmp/t")

        set("//tmp/t/@chunk_merger_mode", "shallow")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        sleep(5)

        wait(lambda: not get("//tmp/t/@is_being_merged"))
        merged_rows = read_table("//tmp/t")
        assert _schematize_rows(rows, schema2) == _schematize_rows(merged_rows, schema2)

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_inherit_chunk_merger_mode(self, merge_mode):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        create("map_node", "//tmp/d")
        set("//tmp/d/@chunk_merger_mode", merge_mode)

        create("table", "//tmp/d/t")
        write_table("<append=true>//tmp/d/t", {"a": "b"})
        write_table("<append=true>//tmp/d/t", {"a": "c"})
        write_table("<append=true>//tmp/d/t", {"a": "d"})

        assert get("//tmp/d/t/@resource_usage/chunk_count") > 1
        info = read_table("//tmp/d/t")

        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/d/t/@resource_usage/chunk_count") == 1)
        assert read_table("//tmp/d/t") == info


class TestChunkMergerMulticell(TestChunkMerger):
    NUM_TEST_PARTITIONS = 6

    NUM_SECONDARY_MASTER_CELLS = 3

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_teleportation(self, merge_mode):
        create("table", "//tmp/t1", attributes={"external_cell_tag" : 2})
        write_table("<append=true>//tmp/t1", {"a": "b"})
        write_table("<append=true>//tmp/t1", {"a": "c"})

        create("table", "//tmp/t2", attributes={"external_cell_tag" : 3})
        write_table("<append=true>//tmp/t2", {"a": "d"})
        write_table("<append=true>//tmp/t2", {"a": "e"})

        create("table", "//tmp/t", attributes={"external_cell_tag" : 3})
        concatenate(["//tmp/t1", "//tmp/t2"], "//tmp/t")

        assert get("//tmp/t1/@external_cell_tag") == 2
        assert get("//tmp/t2/@external_cell_tag") == 3
        assert get("//tmp/t/@external_cell_tag") == 3

        chunk_ids = get("//tmp/t/@chunk_ids")
        remove("//tmp/t1")
        remove("//tmp/t2")

        self._wait_for_merge("//tmp/t", merge_mode, "tmp")

        for chunk_id in chunk_ids:
            wait(lambda: not exists("#{}".format(chunk_id)))


class TestChunkMergerPortal(TestChunkMergerMulticell):
    NUM_TEST_PARTITIONS = 6

    ENABLE_TMP_PORTAL = True
    NUM_SECONDARY_MASTER_CELLS = 3
