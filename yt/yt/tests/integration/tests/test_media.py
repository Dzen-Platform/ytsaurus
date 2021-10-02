from yt_env_setup import YTEnvSetup, Restarter, NODES_SERVICE

from yt_commands import (
    authors, print_debug, wait, create, ls, get, set, exists,
    create_medium, write_file,
    read_table, write_table, write_journal, wait_until_sealed, get_singular_chunk_id, update_nodes_dynamic_config, set_account_disk_space_limit, get_account_disk_space_limit, get_media)

from yt.common import YtError

import pytest

from copy import deepcopy
import __builtin__


################################################################################


class TestMedia(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 10

    NON_DEFAULT_MEDIUM = "hdd2"
    NON_DEFAULT_TRANSIENT_MEDIUM = "hdd3"

    @classmethod
    def setup_class(cls):
        super(TestMedia, cls).setup_class()
        disk_space_limit = get_account_disk_space_limit("tmp", "default")
        set_account_disk_space_limit("tmp", disk_space_limit, TestMedia.NON_DEFAULT_MEDIUM)
        set_account_disk_space_limit("tmp", disk_space_limit, TestMedia.NON_DEFAULT_TRANSIENT_MEDIUM)

    @classmethod
    def modify_node_config(cls, config):
        # Add two more locations with non-default media to each node.
        assert len(config["data_node"]["store_locations"]) == 1
        location_prototype = config["data_node"]["store_locations"][0]

        default_location = deepcopy(location_prototype)
        default_location["path"] += "_0"
        default_location["medium_name"] = "default"

        non_default_location = deepcopy(location_prototype)
        non_default_location["path"] += "_1"
        non_default_location["medium_name"] = cls.NON_DEFAULT_MEDIUM

        non_default_transient_location = deepcopy(location_prototype)
        non_default_transient_location["path"] += "_2"
        non_default_transient_location["medium_name"] = cls.NON_DEFAULT_TRANSIENT_MEDIUM

        config["data_node"]["store_locations"] = []
        config["data_node"]["store_locations"].append(default_location)
        config["data_node"]["store_locations"].append(non_default_location)
        config["data_node"]["store_locations"].append(non_default_transient_location)

    @classmethod
    def on_masters_started(cls):
        create_medium(cls.NON_DEFAULT_MEDIUM)
        create_medium(cls.NON_DEFAULT_TRANSIENT_MEDIUM, attributes={"transient": True})
        medium_count = len(get_media())
        while medium_count < 120:
            create_medium("hdd" + str(medium_count))
            medium_count += 1

    def _check_all_chunks_on_medium(self, tbl, medium):
        chunk_ids = get("//tmp/{0}/@chunk_ids".format(tbl))
        for chunk_id in chunk_ids:
            replicas = get("#" + chunk_id + "/@stored_replicas")
            for replica in replicas:
                if replica.attributes["medium"] != medium:
                    return False
        return True

    def _count_chunks_on_medium(self, tbl, medium):
        chunk_count = 0
        chunk_ids = get("//tmp/{0}/@chunk_ids".format(tbl))
        for chunk_id in chunk_ids:
            replicas = get("#" + chunk_id + "/@stored_replicas")
            for replica in replicas:
                if replica.attributes["medium"] == medium:
                    chunk_count += 1

        return chunk_count

    def _remove_zeros(self, d):
        for k in list(d.keys()):
            if d[k] == 0:
                del d[k]

    def _check_account_and_table_usage_equal(self, tbl):
        account_usage = self._remove_zeros(get("//sys/accounts/tmp/@resource_usage/disk_space_per_medium"))
        table_usage = self._remove_zeros(get("//tmp/{0}/@resource_usage/disk_space_per_medium".format(tbl)))
        return account_usage == table_usage

    def _check_chunk_ok(self, ok, chunk_id, media):
        available = get("#%s/@available" % chunk_id)
        replication_status = get("#%s/@replication_status" % chunk_id)
        lvc = ls("//sys/lost_vital_chunks")
        for medium in media:
            if ok:
                if (
                        not available
                        or medium not in replication_status
                        or replication_status[medium]["underreplicated"]
                        or replication_status[medium]["lost"]
                        or replication_status[medium]["data_missing"]
                        or replication_status[medium]["parity_missing"]
                        or len(lvc) != 0
                ):
                    return False
            else:
                if (
                        available
                        or medium not in replication_status
                        or not replication_status[medium]["lost"]
                        or not replication_status[medium]["data_missing"]
                        or not replication_status[medium]["parity_missing"]
                        or len(lvc) == 0
                ):
                    return False
        return True

    def _get_chunk_replica_nodes(self, chunk_id):
        return __builtin__.set(str(r) for r in get("#{0}/@stored_replicas".format(chunk_id)))

    def _get_chunk_replica_media(self, chunk_id):
        return __builtin__.set(r.attributes["medium"] for r in get("#{0}/@stored_replicas".format(chunk_id)))

    def _ban_nodes(self, nodes):
        banned = False
        for node in ls("//sys/cluster_nodes"):
            if node in nodes:
                set("//sys/cluster_nodes/{0}/@banned".format(node), True)
                banned = True
        assert banned

    @authors()
    def test_default_store_medium_name(self):
        assert get("//sys/media/default/@name") == "default"

    @authors()
    def test_default_store_medium_index(self):
        assert get("//sys/media/default/@index") == 0

    @authors()
    def test_default_cache_medium_name(self):
        assert get("//sys/media/cache/@name") == "cache"

    @authors()
    def test_default_cache_medium_index(self):
        assert get("//sys/media/cache/@index") == 1

    @authors("shakurov")
    def test_create(self):
        assert get("//sys/media/hdd4/@name") == "hdd4"
        assert get("//sys/media/hdd4/@index") > 0

    @authors()
    def test_create_too_many_fails(self):
        with pytest.raises(YtError):
            create_medium("excess_medium")

    @authors("aozeritsky", "shakurov")
    def test_rename(self):
        hdd4_index = get("//sys/media/hdd4/@index")

        set("//sys/media/hdd4/@name", "hdd144")
        assert exists("//sys/media/hdd144")
        assert get("//sys/media/hdd144/@name") == "hdd144"
        assert get("//sys/media/hdd144/@index") == hdd4_index
        assert not exists("//sys/media/hdd4")
        set("//sys/media/hdd144/@name", "hdd4")

    @authors()
    def test_rename_duplicate_name_fails(self):
        with pytest.raises(YtError):
            set("//sys/media/hdd4/@name", "hdd5")

    @authors()
    def test_rename_default_fails(self):
        with pytest.raises(YtError):
            set("//sys/media/default/@name", "new_default")
        with pytest.raises(YtError):
            set("//sys/media/cache/@name", "new_cache")

    @authors()
    def test_create_empty_name_fails(self):
        with pytest.raises(YtError):
            create_medium("")

    @authors()
    def test_create_duplicate_name_fails(self):
        with pytest.raises(YtError):
            create_medium(TestMedia.NON_DEFAULT_MEDIUM)

    @authors("babenko")
    def test_new_table_attributes(self):
        create("table", "//tmp/t1")
        write_table("//tmp/t1", {"a": "b"})

        replication_factor = get("//tmp/t1/@replication_factor")
        assert replication_factor > 0
        assert get("//tmp/t1/@vital")

        assert get("//tmp/t1/@primary_medium") == "default"

        tbl_media = get("//tmp/t1/@media")
        assert tbl_media["default"]["replication_factor"] == replication_factor
        assert not tbl_media["default"]["data_parts_only"]
        wait(lambda: self._check_account_and_table_usage_equal("t1"))

    @authors("shakurov", "babenko")
    def test_assign_additional_medium(self):
        create("table", "//tmp/t2")
        write_table("//tmp/t2", {"a": "b"})

        tbl_media = get("//tmp/t2/@media")
        tbl_media[TestMedia.NON_DEFAULT_MEDIUM] = {
            "replication_factor": 7,
            "data_parts_only": False,
        }

        set("//tmp/t2/@media", tbl_media)

        tbl_media = get("//tmp/t2/@media")
        assert tbl_media[TestMedia.NON_DEFAULT_MEDIUM]["replication_factor"] == 7
        assert not tbl_media[TestMedia.NON_DEFAULT_MEDIUM]["data_parts_only"]

        set("//tmp/t2/@primary_medium", TestMedia.NON_DEFAULT_MEDIUM)

        assert get("//tmp/t2/@primary_medium") == TestMedia.NON_DEFAULT_MEDIUM
        assert get("//tmp/t2/@replication_factor") == 7

        wait(
            lambda:
                self._count_chunks_on_medium("t2", "default") == 3
                and self._count_chunks_on_medium("t2", TestMedia.NON_DEFAULT_MEDIUM) == 7
                and self._check_account_and_table_usage_equal("t2"))

    @authors("babenko")
    def test_move_between_media(self):
        create("table", "//tmp/t3")
        write_table("//tmp/t3", {"a": "b"})

        tbl_media = get("//tmp/t3/@media")
        tbl_media[TestMedia.NON_DEFAULT_MEDIUM] = {
            "replication_factor": 3,
            "data_parts_only": False,
        }

        set("//tmp/t3/@media", tbl_media)
        set("//tmp/t3/@primary_medium", TestMedia.NON_DEFAULT_MEDIUM)

        tbl_media["default"] = {"replication_factor": 0, "data_parts_only": False}
        set("//tmp/t3/@media", tbl_media)

        tbl_media = get("//tmp/t3/@media")
        assert "default" not in tbl_media
        assert tbl_media[TestMedia.NON_DEFAULT_MEDIUM]["replication_factor"] == 3
        assert not tbl_media[TestMedia.NON_DEFAULT_MEDIUM]["data_parts_only"]

        assert get("//tmp/t3/@primary_medium") == TestMedia.NON_DEFAULT_MEDIUM
        assert get("//tmp/t3/@replication_factor") == 3

        wait(
            lambda:
                self._check_all_chunks_on_medium("t3", TestMedia.NON_DEFAULT_MEDIUM)
                and self._check_account_and_table_usage_equal("t3"))

    @authors("babenko")
    def test_move_between_media_shortcut(self):
        create("table", "//tmp/t4")
        write_table("//tmp/t4", {"a": "b"})

        # Shortcut: making a previously disabled medium primary *moves* the table to that medium.
        set("//tmp/t4/@primary_medium", TestMedia.NON_DEFAULT_MEDIUM)

        tbl_media = get("//tmp/t4/@media")
        assert "default" not in tbl_media
        assert tbl_media[TestMedia.NON_DEFAULT_MEDIUM]["replication_factor"] == 3
        assert not tbl_media[TestMedia.NON_DEFAULT_MEDIUM]["data_parts_only"]

        assert get("//tmp/t4/@primary_medium") == TestMedia.NON_DEFAULT_MEDIUM
        assert get("//tmp/t4/@replication_factor") == 3

        wait(
            lambda:
                self._check_all_chunks_on_medium("t4", TestMedia.NON_DEFAULT_MEDIUM)
                and self._check_account_and_table_usage_equal("t4"))

    @authors("shakurov")
    def test_assign_empty_medium_fails(self):
        create("table", "//tmp/t5")
        write_table("//tmp/t5", {"a": "b"})

        empty_tbl_media = {"default": {"replication_factor": 0, "data_parts_only": False}}
        with pytest.raises(YtError):
            set("//tmp/t5/@media", empty_tbl_media)

        parity_loss_tbl_media = {"default": {"replication_factor": 3, "data_parts_only": True}}
        with pytest.raises(YtError):
            set("//tmp/t5/@media", parity_loss_tbl_media)

    @authors("babenko", "shakurov")
    def test_write_to_non_default_medium(self):
        create("table", "//tmp/t6")
        # Move table into non-default medium.
        set("//tmp/t6/@primary_medium", TestMedia.NON_DEFAULT_MEDIUM)
        write_table(
            "<append=true>//tmp/t6",
            {"a": "b"},
            table_writer={"medium_name": TestMedia.NON_DEFAULT_MEDIUM},
        )

        wait(
            lambda:
                self._check_all_chunks_on_medium("t6", TestMedia.NON_DEFAULT_MEDIUM)
                and self._check_account_and_table_usage_equal("t6"))

    @authors("shakurov", "babenko")
    def test_chunks_inherit_properties(self):
        create("table", "//tmp/t7")
        write_table("//tmp/t7", {"a": "b"})

        tbl_media_1 = get("//tmp/t7/@media")
        tbl_vital_1 = get("//tmp/t7/@vital")

        chunk_id = get_singular_chunk_id("//tmp/t7")

        chunk_media_1 = deepcopy(tbl_media_1)
        chunk_vital_1 = tbl_vital_1

        assert chunk_media_1 == get("#" + chunk_id + "/@media")
        assert chunk_vital_1 == get("#" + chunk_id + "/@vital")

        # Modify table properties in some way.
        tbl_media_2 = deepcopy(tbl_media_1)
        tbl_vital_2 = not tbl_vital_1
        tbl_media_2["hdd6"] = {"replication_factor": 7, "data_parts_only": True}

        set("//tmp/t7/@media", tbl_media_2)
        set("//tmp/t7/@vital", tbl_vital_2)

        assert tbl_media_2 == get("//tmp/t7/@media")
        assert tbl_vital_2 == get("//tmp/t7/@vital")

        chunk_media_2 = deepcopy(tbl_media_2)
        chunk_vital_2 = tbl_vital_2

        wait(
            lambda:
                chunk_media_2 == get("#" + chunk_id + "/@media")
                and chunk_vital_2 == get("#" + chunk_id + "/@vital"))

    @authors("babenko")
    def test_no_create_cache_media(self):
        with pytest.raises(YtError):
            create_medium("new_cache", attributes={"cache": True})

    @authors("shakurov")
    def test_chunks_intersecting_by_nodes(self):
        create("table", "//tmp/t8")
        write_table("//tmp/t8", {"a": "b"})

        tbl_media = get("//tmp/t8/@media")
        # Forcing chunks to be placed twice on one node (once for each medium) by making
        # replication factor equal to the number of nodes.
        tbl_media["default"]["replication_factor"] = TestMedia.NUM_NODES
        tbl_media[TestMedia.NON_DEFAULT_MEDIUM] = {
            "replication_factor": TestMedia.NUM_NODES,
            "data_parts_only": False,
        }

        set("//tmp/t8/@media", tbl_media)

        assert get("//tmp/t8/@replication_factor") == TestMedia.NUM_NODES
        assert get("//tmp/t8/@media/default/replication_factor") == TestMedia.NUM_NODES
        assert get("//tmp/t8/@media/{}/replication_factor".format(TestMedia.NON_DEFAULT_MEDIUM)) == TestMedia.NUM_NODES

        wait(
            lambda:
                self._count_chunks_on_medium("t8", "default") == TestMedia.NUM_NODES
                and self._count_chunks_on_medium("t8", TestMedia.NON_DEFAULT_MEDIUM) == TestMedia.NUM_NODES)

    @authors("shakurov")
    def test_default_media_priorities(self):
        assert get("//sys/media/default/@priority") == 0
        assert get("//sys/media/cache/@priority") == 0

    @authors("shakurov")
    def test_new_medium_default_priority(self):
        assert get("//sys/media/{}/@priority".format(TestMedia.NON_DEFAULT_MEDIUM)) == 0

    @authors("shakurov")
    def test_set_medium_priority(self):
        assert get("//sys/media/hdd4/@priority") == 0
        set("//sys/media/hdd4/@priority", 7)
        assert get("//sys/media/hdd4/@priority") == 7
        set("//sys/media/hdd4/@priority", 10)
        assert get("//sys/media/hdd4/@priority") == 10
        set("//sys/media/hdd4/@priority", 0)
        assert get("//sys/media/hdd4/@priority") == 0

    @authors("shakurov")
    def test_set_incorrect_medium_priority(self):
        assert get("//sys/media/hdd5/@priority") == 0
        with pytest.raises(YtError):
            set("//sys/media/hdd5/@priority", 11)
        with pytest.raises(YtError):
            set("//sys/media/hdd5/@priority", -1)

    @authors("babenko")
    def test_journal_medium(self):
        create("journal", "//tmp/j", attributes={"primary_medium": self.NON_DEFAULT_MEDIUM})
        assert exists("//tmp/j/@media/{0}".format(self.NON_DEFAULT_MEDIUM))
        data = [{"data": "payload" + str(i)} for i in xrange(0, 10)]
        write_journal("//tmp/j", data)
        wait_until_sealed("//tmp/j")
        chunk_id = get_singular_chunk_id("//tmp/j")
        for replica in get("#{0}/@stored_replicas".format(chunk_id)):
            assert replica.attributes["medium"] == self.NON_DEFAULT_MEDIUM

    @authors("babenko")
    def test_file_medium(self):
        create("file", "//tmp/f", attributes={"primary_medium": self.NON_DEFAULT_MEDIUM})
        assert exists("//tmp/f/@media/{0}".format(self.NON_DEFAULT_MEDIUM))
        write_file("//tmp/f", "payload")
        chunk_id = get_singular_chunk_id("//tmp/f")
        for replica in get("#{0}/@stored_replicas".format(chunk_id)):
            assert replica.attributes["medium"] == self.NON_DEFAULT_MEDIUM

    @authors("babenko")
    def test_table_medium(self):
        create("table", "//tmp/t", attributes={"primary_medium": self.NON_DEFAULT_MEDIUM})
        assert exists("//tmp/t/@media/{0}".format(self.NON_DEFAULT_MEDIUM))
        write_table("//tmp/t", [{"key": "value"}])
        chunk_id = get_singular_chunk_id("//tmp/t")
        for replica in get("#{0}/@stored_replicas".format(chunk_id)):
            assert replica.attributes["medium"] == self.NON_DEFAULT_MEDIUM

    @authors("babenko", "shakurov")
    def test_chunk_statuses_1_media(self):
        codec = "reed_solomon_6_3"
        codec_replica_count = 9

        create("table", "//tmp/t9")
        set("//tmp/t9/@erasure_codec", codec)
        write_table("//tmp/t9", {"a": "b"})

        assert get("//tmp/t9/@chunk_count") == 1
        chunk_id = get_singular_chunk_id("//tmp/t9")

        wait(
            lambda:
                self._check_chunk_ok(True, chunk_id, {"default"})
                and len(get("#{0}/@stored_replicas".format(chunk_id))) == codec_replica_count
                and self._get_chunk_replica_media(chunk_id) == {"default"})

        self._ban_nodes(self._get_chunk_replica_nodes(chunk_id))

        wait(lambda: self._check_chunk_ok(False, chunk_id, {"default"}))

    @authors("shakurov")
    def test_chunk_statuses_2_media(self):
        codec = "reed_solomon_6_3"
        codec_replica_count = 9

        create("table", "//tmp/t9")
        set("//tmp/t9/@erasure_codec", codec)
        write_table("//tmp/t9", {"a": "b"})

        tbl_media = get("//tmp/t9/@media")
        tbl_media[TestMedia.NON_DEFAULT_MEDIUM] = {
            "replication_factor": 3,
            "data_parts_only": False,
        }
        set("//tmp/t9/@media", tbl_media)

        relevant_media = {"default", TestMedia.NON_DEFAULT_MEDIUM}

        assert get("//tmp/t9/@chunk_count") == 1
        chunk_id = get_singular_chunk_id("//tmp/t9")

        wait(
            lambda:
                self._check_chunk_ok(True, chunk_id, relevant_media)
                and len(get("#{0}/@stored_replicas".format(chunk_id))) == len(relevant_media) * codec_replica_count
                and self._get_chunk_replica_media(chunk_id) == relevant_media)

        self._ban_nodes(self._get_chunk_replica_nodes(chunk_id))

        wait(lambda: self._check_chunk_ok(False, chunk_id, relevant_media))

    @authors("babenko")
    def test_create_with_invalid_attrs_yt_7093(self):
        with pytest.raises(YtError):
            create_medium("x", attributes={"priority": "hello"})
        assert not exists("//sys/media/x")

    @authors("shakurov")
    def test_transient_only_chunks_not_vital_yt_9133(self):
        create(
            "table",
            "//tmp/t10_persistent",
            attributes={
                "primary_medium": self.NON_DEFAULT_MEDIUM,
                "media": {
                    self.NON_DEFAULT_MEDIUM: {
                        "replication_factor": 2,
                        "data_parts_only": False,
                    }
                },
            },
        )
        create(
            "table",
            "//tmp/t10_transient",
            attributes={
                "primary_medium": self.NON_DEFAULT_TRANSIENT_MEDIUM,
                "media": {
                    self.NON_DEFAULT_TRANSIENT_MEDIUM: {
                        "replication_factor": 2,
                        "data_parts_only": False,
                    }
                },
            },
        )
        write_table("//tmp/t10_persistent", {"a": "b"})
        write_table("//tmp/t10_transient", {"a": "b"})

        chunk1 = get_singular_chunk_id("//tmp/t10_persistent")
        chunk2 = get_singular_chunk_id("//tmp/t10_transient")

        wait(
            lambda:
                len(get("#{0}/@stored_replicas".format(chunk1))) == 2
                and len(get("#{0}/@stored_replicas".format(chunk2))) == 2)

        set("//sys/@config/chunk_manager/enable_chunk_replicator", False, recursive=True)
        wait(lambda: not get("//sys/@chunk_replicator_enabled"))

        chunk1_nodes = get("#{0}/@stored_replicas".format(chunk1))
        chunk2_nodes = get("#{0}/@stored_replicas".format(chunk2))

        for node in chunk1_nodes + chunk2_nodes:
            set("//sys/cluster_nodes/{0}/@banned".format(node), True)

        set("//sys/@config/chunk_manager/enable_chunk_replicator", True)
        wait(lambda: get("//sys/@chunk_replicator_enabled"))

        def persistent_chunk_is_lost_vital():
            lvc = ls("//sys/lost_vital_chunks")
            return chunk1 in lvc

        def transient_chunk_is_lost_but_not_vital():
            lc = ls("//sys/lost_chunks")
            lvc = ls("//sys/lost_vital_chunks")
            return chunk2 in lc and chunk2 not in lvc

        wait(persistent_chunk_is_lost_vital)
        wait(transient_chunk_is_lost_but_not_vital)

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_merge_chunks_for_nondefault_medium(self, merge_mode):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        primary_medium = TestMedia.NON_DEFAULT_MEDIUM

        create("table", "//tmp/t", attributes={"primary_medium": primary_medium})

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        info = read_table("//tmp/t")

        set("//tmp/t/@chunk_merger_mode", merge_mode)
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)
        assert read_table("//tmp/t") == info

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@requisition/0/medium".format(chunk_ids[0])) == primary_medium

    @authors("aleksandra-zh")
    @pytest.mark.parametrize("merge_mode", ["deep", "shallow"])
    def test_merge_chunks_change_medium(self, merge_mode):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        primary_medium = TestMedia.NON_DEFAULT_MEDIUM

        create("table", "//tmp/t", attributes={"primary_medium": primary_medium})

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        info = read_table("//tmp/t")

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@requisition/0/medium".format(chunk_ids[0])) == primary_medium

        another_primary_medium = TestMedia.NON_DEFAULT_TRANSIENT_MEDIUM
        set("//tmp/t/@primary_medium", another_primary_medium)

        set("//tmp/t/@chunk_merger_mode", merge_mode)
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)
        assert read_table("//tmp/t") == info

        chunk_ids = get("//tmp/t/@chunk_ids")
        assert get("#{0}/@requisition/0/medium".format(chunk_ids[0])) == another_primary_medium

    @authors("aleksandra-zh")
    def test_merge_chunks_with_two_media(self):
        set("//sys/@config/chunk_manager/chunk_merger/enable", True)

        create("table", "//tmp/t")

        tbl_media = get("//tmp/t/@media")
        tbl_media[TestMedia.NON_DEFAULT_MEDIUM] = {
            "replication_factor": 2,
            "data_parts_only": False
        }
        set("//tmp/t/@media", tbl_media)

        write_table("<append=true>//tmp/t", {"a": "b"})
        write_table("<append=true>//tmp/t", {"a": "c"})
        write_table("<append=true>//tmp/t", {"a": "d"})

        set("//tmp/t/@chunk_merger_mode", "deep")
        set("//sys/accounts/tmp/@merge_job_rate_limit", 10)
        set("//sys/accounts/tmp/@chunk_merger_node_traversal_concurrency", 1)

        wait(lambda: get("//tmp/t/@resource_usage/chunk_count") == 1)

        chunk_ids = get("//tmp/t/@chunk_ids")
        def are_requisitions_correct():
            requisitions = get("#{0}/@requisition".format(chunk_ids[0]))
            return [r["medium"] for r in requisitions] == ["default", TestMedia.NON_DEFAULT_MEDIUM]
        wait(are_requisitions_correct)


################################################################################


class TestMediaMulticell(TestMedia):
    NUM_SECONDARY_MASTER_CELLS = 2


################################################################################


class TestDynamicMedia(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 2

    DELTA_NODE_CONFIG = {
        "dynamic_config_manager": {
            "enable_unrecognized_options_alert": True,
        },
        "cluster_connection": {
            "medium_directory_synchronizer": {
                "sync_period": 1
            }
        }
    }

    def setup(self):
        update_nodes_dynamic_config(
            {
                "data_node": {
                    "medium_updater":
                        {
                            "enabled": True,
                            "period": 1
                        }
                }
            }
        )

    @authors("s-v-m")
    def test_medium_change_simple(self):
        for i in range(10):
            table = "//tmp/t{}".format(i)
            create("table", table, attributes={"replication_factor": 1})
            write_table(table, {"foo": "bar"})
        node = ls("//sys/cluster_nodes")[0]
        location_uuid = get("//sys/cluster_nodes/{0}/@statistics/locations/0/location_uuid".format(node))
        print_debug(get("//sys/cluster_nodes/{0}/@statistics/locations".format(node)))
        assert get("//sys/cluster_nodes/{0}/@statistics/locations/0/chunk_count".format(node)) > 0

        medium_name = "testmedium"
        create_medium(medium_name)
        set("//sys/cluster_nodes/{0}/@config".format(node), {
            "medium_overrides": {
                location_uuid: medium_name
            }
        })
        wait(lambda: get("//sys/cluster_nodes/{0}/@statistics/locations/0/medium_name".format(node)) == medium_name,
             ignore_exceptions=True)
        wait(lambda: get("//sys/cluster_nodes/{0}/@statistics/locations/0/chunk_count".format(node)) == 0, iter=100)

        with Restarter(self.Env, NODES_SERVICE):
            pass

        assert get("//sys/cluster_nodes/{0}/@statistics/locations/0/medium_name".format(node)) == medium_name

        set("//sys/cluster_nodes/{0}/@config".format(node), {})
        wait(lambda: get("//sys/cluster_nodes/{0}/@statistics/locations/0/medium_name".format(node)) == "default",
             ignore_exceptions=True)

        with Restarter(self.Env, NODES_SERVICE):
            pass

        assert get("//sys/cluster_nodes/{0}/@statistics/locations/0/medium_name".format(node)) == "default"
