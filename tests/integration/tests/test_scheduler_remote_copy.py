import pytest

from yt_env_setup import YTEnvSetup, make_schema
from yt_commands import *
from yt.environment import YTInstance

import time


##################################################################

class TestSchedulerRemoteCopyCommands(YTEnvSetup):
    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "cluster_directory_update_period": 500
        }
    }

    NUM_MASTERS = 3
    NUM_NODES = 9
    NUM_SCHEDULERS = 1

    @classmethod
    def setup_class(cls, secondary_master_cell_count=0):
        super(TestSchedulerRemoteCopyCommands, cls).setup_class()
        cls.remote_env = YTInstance(
            os.path.join(cls.Env.path, "_remote"),
            master_count=1,
            nonvoting_master_count=0,
            node_count=9,
            secondary_master_cell_count=secondary_master_cell_count,
            scheduler_count=0,
            has_proxy=False,
            cell_tag=10,
            port_locks_path=cls.Env.port_locks_path,
            modify_configs_func=cls.apply_config_patches)
        cls.remote_env.start(start_secondary_master_cells=cls.START_SECONDARY_MASTER_CELLS)

    def setup(self):
        set("//sys/clusters/remote",
            {
                "primary_master": self.remote_env.configs["master"][0]["primary_master"],
                "secondary_masters": self.remote_env.configs["master"][0]["secondary_masters"],
                "timestamp_provider": self.remote_env.configs["master"][0]["timestamp_provider"],
                "transaction_manager": self.remote_env.configs["master"][0]["transaction_manager"],
                "cell_tag": 10
            })
        self.remote_driver = Driver(config=self.remote_env.configs["driver"])
        time.sleep(1.0)

    def teardown(self):
        set("//tmp", {}, driver=self.remote_driver)


    def test_empty_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert read_table("//tmp/t2") == []
        assert not get("//tmp/t2/@sorted")

    def test_non_empty_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert read_table("//tmp/t2") == [{"a": "b"}]
        assert not get("//tmp/t2/@sorted")

    def test_cluster_connection_config(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        cluster_connection = get("//sys/clusters/remote")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_connection": cluster_connection})

        assert read_table("//tmp/t2") == [{"a": "b"}]

    def test_multi_chunk_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"a": "b"}, driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"c": "d"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert sorted(read_table("//tmp/t2")) == [{"a": "b"}, {"c": "d"}]
        assert get("//tmp/t2/@chunk_count") == 2

    def test_multiple_jobs(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"a": "b"}, driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"c": "d"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote", "job_count": 2})

        assert sorted(read_table("//tmp/t2")) == [{"a": "b"}, {"c": "d"}]
        assert get("//tmp/t2/@chunk_count") == 2

    def test_heterogenius_chunk_in_one_job(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"a": "b"}, driver=self.remote_driver)
        set("//tmp/t1/@erasure_codec", "reed_solomon_6_3", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"c": "d"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert read_table("//tmp/t2") == [{"a": "b"}, {"c": "d"}]

    def test_sorted_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", [{"a": "b"}, {"a": "c"}], sorted_by="a", driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert read_table("//tmp/t2") == [{"a": "b"}, {"a": "c"}]
        assert get("//tmp/t2/@sorted")
        assert get("//tmp/t2/@sorted_by") == ["a"]

    def test_erasure_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        set("//tmp/t1/@erasure_codec", "reed_solomon_6_3", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert read_table("//tmp/t2") == [{"a": "b"}]

    def test_chunk_scraper(self):
        def set_banned_flag(value):
            if value:
                flag = True
                state = "offline"
            else:
                flag = False
                state = "online"

            address = get("//sys/nodes", driver=self.remote_driver).keys()[0]
            set("//sys/nodes/%s/@banned" % address, flag, driver=self.remote_driver)

            # Give it enough time to register or unregister the node
            time.sleep(1.0)
            assert get("//sys/nodes/%s/@state" % address, driver=self.remote_driver) == state

        create("table", "//tmp/t1", driver=self.remote_driver)
        set("//tmp/t1/@erasure_codec", "reed_solomon_6_3", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        set_banned_flag(True)

        time.sleep(1)

        create("table", "//tmp/t2")
        op = remote_copy(dont_track=True, in_="//tmp/t1", out="//tmp/t2",
                            spec={"cluster_name": "remote",
                                  "unavailable_chunk_strategy": "wait",
                                  "network_name": "interconnect"})

        time.sleep(1)
        set_banned_flag(False)

        op.track()

        assert read_table("//tmp/t2") == [{"a": "b"}]

    def test_revive(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        op = remote_copy(dont_track=True, in_="//tmp/t1", out="//tmp/t2",
                            spec={"cluster_name": "remote"})

        self.Env.kill_schedulers()
        time.sleep(1)
        self.Env.start_schedulers()

        op.track()

        assert read_table("//tmp/t2") == [{"a" : "b"}]

    def test_failed_cases(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        with pytest.raises(YtError):
            remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "unexisting"})

        with pytest.raises(YtError):
            remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote", "network_name": "unexisting"})

        with pytest.raises(YtError):
            remote_copy(in_="//tmp/t1", out="//tmp/unexisting", spec={"cluster_name": "remote"})

        write_table("//tmp/t1", [{"a": "b"}, {"c": "d"}], driver=self.remote_driver)
        with pytest.raises(YtError):
            remote_copy(in_="//tmp/t1[:#1]", out="//tmp/unexisting", spec={"cluster_name": "remote"})

    def test_acl(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")

        create_user("u")
        create_user("u", driver=self.remote_driver)

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"}, user="u")

        set("//tmp/t1/@acl/end", {"action": "deny", "subjects": ["u"], "permissions": ["read"]}, driver=self.remote_driver)
        with pytest.raises(YtError):
            remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"}, user="u")
        set("//tmp/t1/@acl", [], driver=self.remote_driver)

        set("//sys/schemas/transaction/@acl/end", {"action": "deny", "subjects": ["u"], "permissions": ["create"]}, driver=self.remote_driver)
        with pytest.raises(YtError):
            remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"}, user="u")

    def test_copy_attributes(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        set("//tmp/t1/@custom_attr1", "attr_value1", driver=self.remote_driver)
        set("//tmp/t1/@custom_attr2", "attr_value2", driver=self.remote_driver)

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote", "copy_attributes": True})

        assert get("//tmp/t2/@custom_attr1") == "attr_value1"
        assert get("//tmp/t2/@custom_attr2") == "attr_value2"

        remote_copy(in_="//tmp/t1", out="//tmp/t3", spec={"cluster_name": "remote", "copy_attributes": True, "attribute_keys": ["custom_attr2"]})
        assert not exists("//tmp/t3/@custom_attr1")
        assert get("//tmp/t3/@custom_attr2") == "attr_value2"

        with pytest.raises(YtError):
            remote_copy(in_=["//tmp/t1", "//tmp/t1"], out="//tmp/t2", spec={"cluster_name": "remote", "copy_attributes": True})

    def test_copy_strict_schema(self):
        create("table", "//tmp/t1", driver=self.remote_driver, attributes={"schema":
            make_schema([
                {"name": "a", "type": "string", "sort_order": "ascending"},
                {"name": "b", "type": "string"}],
                unique_keys=True)
            })
        assert get("//tmp/t1/@schema_mode", driver=self.remote_driver) == "strong"

        create("table", "//tmp/t2")

        rows = [{"a": "x", "b": "v"}, {"a": "y", "b": "v"}]
        write_table("//tmp/t1", rows, driver=self.remote_driver)

        assert get("//tmp/t1/@schema_mode", driver=self.remote_driver) == "strong"

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert read_table("//tmp/t2") == rows
        assert get("//tmp/t2/@schema/@strict")
        assert get("//tmp/t2/@schema/@unique_keys")
        assert get("//tmp/t2/@schema_mode") == "strong"

class TestSchedulerRemoteCopyNetworks(YTEnvSetup):
    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "cluster_directory_update_period": 500
        }
    }

    NUM_MASTERS = 3
    NUM_NODES = 9
    NUM_SCHEDULERS = 1

    @classmethod
    def modify_node_config(cls, config):
        config["addresses"]["custom_network"] = config["addresses"]["default"]

    @classmethod
    def setup_class(cls, secondary_master_cell_count=0):
        super(TestSchedulerRemoteCopyNetworks, cls).setup_class()
        cls.remote_env = YTInstance(
            os.path.join(cls.Env.path, "_remote"),
            master_count=1,
            nonvoting_master_count=0,
            node_count=9,
            secondary_master_cell_count=secondary_master_cell_count,
            scheduler_count=0,
            has_proxy=False,
            cell_tag=10,
            port_locks_path=cls.Env.port_locks_path,
            modify_configs_func=cls.apply_config_patches)
        cls.remote_env.start(start_secondary_master_cells=cls.START_SECONDARY_MASTER_CELLS)

    def setup(self):
        set("//sys/clusters/remote",
            {
                "primary_master": self.remote_env.configs["master"][0]["primary_master"],
                "secondary_masters": self.remote_env.configs["master"][0]["secondary_masters"],
                "timestamp_provider": self.remote_env.configs["master"][0]["timestamp_provider"],
                "transaction_manager": self.remote_env.configs["master"][0]["transaction_manager"],
                "cell_tag": 10
            })
        self.remote_driver = Driver(config=self.remote_env.configs["driver"])
        time.sleep(1.0)

    def teardown(self):
        set("//tmp", {}, driver=self.remote_driver)

    def test_default_network(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote"})

        assert read_table("//tmp/t2") == [{"a": "b"}]

    def test_custom_network(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "remote", "network_name": "custom_network"})

        assert read_table("//tmp/t2") == [{"a": "b"}]

##################################################################

class TestSchedulerRemoteCopyCommandsMulticell(TestSchedulerRemoteCopyCommands):
    NUM_SECONDARY_MASTER_CELLS = 2

    @classmethod
    def setup_class(cls):
        super(TestSchedulerRemoteCopyCommandsMulticell, cls).setup_class(2)

