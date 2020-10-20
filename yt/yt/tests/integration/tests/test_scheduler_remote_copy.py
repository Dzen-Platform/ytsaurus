import pytest

from yt_env_setup import (
    YTEnvSetup,
    wait,
    Restarter,
    SCHEDULERS_SERVICE,
    CONTROLLER_AGENTS_SERVICE,
)
from yt_commands import *
from yt import yson

import time


##################################################################


class TestSchedulerRemoteCopyCommands(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 9
    NUM_SCHEDULERS = 1

    NUM_REMOTE_CLUSTERS = 1

    NUM_MASTERS_REMOTE_0 = 1
    NUM_SCHEDULERS_REMOTE_0 = 0

    REMOTE_CLUSTER_NAME = "remote_0"

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "snapshot_period": 500,
        }
    }

    @classmethod
    def setup_class(cls):
        super(TestSchedulerRemoteCopyCommands, cls).setup_class()
        cls.remote_driver = get_driver(cluster=cls.REMOTE_CLUSTER_NAME)

    @authors("ignat")
    def test_empty_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == []
        assert not get("//tmp/t2/@sorted")

    @authors("ignat")
    def test_non_empty_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == [{"a": "b"}]
        assert not get("//tmp/t2/@sorted")

    @authors("asaitgalin", "babenko")
    def test_schema_inference(self):
        schema = make_schema(
            [{"name": "a", "type": "string", "required": False}],
            strict=True,
            unique_keys=False,
        )

        create(
            "table",
            "//tmp/t1",
            attributes={"schema": schema},
            driver=self.remote_driver,
        )

        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")
        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == [{"a": "b"}]
        assert normalize_schema(get("//tmp/t2/@schema")) == schema
        assert get("//tmp/t2/@schema_mode") == "strong"

        create(
            "table",
            "//tmp/t3",
            attributes={"schema": [{"name": "b", "type": "string"}]},
        )

        with pytest.raises(YtError):
            # To do remote copy into table with "stong" schema mode schemas must be identical.
            remote_copy(
                in_="//tmp/t1",
                out="//tmp/t3",
                spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
            )

        with pytest.raises(YtError):
            # To do remote copy into table with "stong" schema mode schemas must be identical.
            # Even if we force scheduler to infer schema from output.
            remote_copy(
                in_="//tmp/t1",
                out="//tmp/t3",
                spec={
                    "cluster_name": self.REMOTE_CLUSTER_NAME,
                    "schema_inference_mode": "from_output",
                },
            )

    @authors("ermolovd")
    def test_schema_validation_complex_types(self):
        input_schema = make_schema(
            [
                {"name": "index", "type_v3": "int64"},
                {"name": "value", "type_v3": optional_type(optional_type("string"))},
            ],
            unique_keys=False,
            strict=True,
        )
        output_schema = make_schema(
            [
                {"name": "index", "type_v3": "int64"},
                {"name": "value", "type_v3": list_type(optional_type("string"))},
            ],
            unique_keys=False,
            strict=True,
        )

        create(
            "table",
            "//tmp/input",
            attributes={"schema": input_schema},
            driver=self.remote_driver,
        )
        create("table", "//tmp/output", attributes={"schema": output_schema})
        write_table(
            "//tmp/input",
            [
                {"index": 1, "value": [None]},
                {"index": 2, "value": ["foo"]},
            ],
            driver=self.remote_driver,
        )

        # We check that yson representation of types are compatible with each other
        write_table(
            "//tmp/output", read_table("//tmp/input", driver=self.remote_driver)
        )

        with pytest.raises(YtError):
            remote_copy(
                in_="//tmp/input",
                out="//tmp/output",
                spec={
                    "cluster_name": self.REMOTE_CLUSTER_NAME,
                    "schema_inference_mode": "auto",
                },
            )
        remote_copy(
            in_="//tmp/input",
            out="//tmp/output",
            spec={
                "cluster_name": self.REMOTE_CLUSTER_NAME,
                "schema_inference_mode": "from_input",
            },
        )
        assert normalize_schema_v3(input_schema) == normalize_schema_v3(
            get("//tmp/output/@schema")
        )

    @authors("ignat")
    def test_cluster_connection_config(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        cluster_connection = get("//sys/clusters/" + self.REMOTE_CLUSTER_NAME)

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_connection": cluster_connection},
        )

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @authors("ignat")
    def test_multi_chunk_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"a": "b"}, driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"c": "d"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert sorted(read_table("//tmp/t2")) == [{"a": "b"}, {"c": "d"}]
        assert get("//tmp/t2/@chunk_count") == 2

    @authors("ignat")
    def test_multi_chunk_sorted_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        for value in xrange(10):
            write_table(
                "<append=true;sorted_by=[a]>//tmp/t1",
                {"a": value},
                driver=self.remote_driver,
            )

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME, "job_count": 10},
        )

        assert read_table("//tmp/t2") == [{"a": value} for value in xrange(10)]
        assert get("//tmp/t2/@chunk_count") == 10

    @authors("ignat")
    def test_multiple_jobs(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"a": "b"}, driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"c": "d"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME, "job_count": 2},
        )

        assert sorted(read_table("//tmp/t2")) == [{"a": "b"}, {"c": "d"}]
        assert get("//tmp/t2/@chunk_count") == 2

    @authors("ignat")
    def test_heterogenius_chunk_in_one_job(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"a": "b"}, driver=self.remote_driver)
        set("//tmp/t1/@erasure_codec", "reed_solomon_6_3", driver=self.remote_driver)
        write_table("<append=true>//tmp/t1", {"c": "d"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == [{"a": "b"}, {"c": "d"}]

    @authors("ignat")
    def test_sorted_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table(
            "//tmp/t1",
            [{"a": "b"}, {"a": "c"}],
            sorted_by="a",
            driver=self.remote_driver,
        )

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == [{"a": "b"}, {"a": "c"}]
        assert get("//tmp/t2/@sorted")
        assert get("//tmp/t2/@sorted_by") == ["a"]

    @authors("ignat")
    def test_erasure_table(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        set("//tmp/t1/@erasure_codec", "reed_solomon_6_3", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @authors("ignat")
    def test_chunk_scraper(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        set("//tmp/t1/@erasure_codec", "reed_solomon_6_3", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        chunk_id = get("//tmp/t1/@chunk_ids/0", driver=self.remote_driver)
        chunk_replicas = get(
            "#{}/@stored_replicas".format(chunk_id), driver=self.remote_driver
        )
        node = list(str(r) for r in chunk_replicas if r.attributes["index"] == 0)[0]

        set(
            "//sys/@config/chunk_manager/enable_chunk_replicator",
            False,
            driver=self.remote_driver,
        )
        multicell_sleep()

        set_banned_flag(True, [node], driver=self.remote_driver)

        wait(
            lambda: not get(
                "#{}/@available".format(chunk_id), driver=self.remote_driver
            )
        )

        create("table", "//tmp/t2")
        op = remote_copy(
            track=False,
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={
                "cluster_name": self.REMOTE_CLUSTER_NAME,
                "unavailable_chunk_strategy": "wait",
                "network_name": "interconnect",
            },
        )

        set_banned_flag(False, [node], driver=self.remote_driver)

        wait(lambda: get("#{}/@available".format(chunk_id), driver=self.remote_driver))

        op.track()

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @authors("ignat")
    def test_revive(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        op = remote_copy(
            track=False,
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={
                "cluster_name": self.REMOTE_CLUSTER_NAME,
                "job_io": {
                    "table_writer": {
                        "testing_delay": 5000,
                    }
                },
            },
        )

        wait(lambda: op.get_state() == "running")
        wait(lambda: exists(op.get_path() + "/snapshot"))

        input_tx = get(op.get_path() + "/@input_transaction_id")

        with Restarter(self.Env, [SCHEDULERS_SERVICE, CONTROLLER_AGENTS_SERVICE]):
            time.sleep(1)

        op.track()

        assert input_tx == get(op.get_path() + "/@input_transaction_id")

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @authors("ignat")
    def test_revive_with_specified_connection(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table(
            "//tmp/t1",
            [{"a": i} for i in xrange(100)],
            max_row_buffer_size=1,
            table_writer={"desired_chunk_size": 1},
            driver=self.remote_driver,
        )

        create("table", "//tmp/t2")

        clusters = get("//sys/clusters")
        cluster_connection = clusters[self.REMOTE_CLUSTER_NAME]
        try:
            set("//sys/clusters", {})
            # TODO(babenko): wait for cluster sync
            time.sleep(2)
            op = remote_copy(
                track=False,
                in_="//tmp/t1",
                out="//tmp/t2",
                spec={"cluster_connection": cluster_connection, "job_count": 100},
            )

            wait(lambda: op.get_state() == "running")
            wait(lambda: exists(op.get_path() + "/snapshot"))

            input_tx = get(op.get_path() + "/@input_transaction_id")

            with Restarter(self.Env, [SCHEDULERS_SERVICE, CONTROLLER_AGENTS_SERVICE]):
                time.sleep(1)

            op.track()

            assert input_tx != get(op.get_path() + "/@input_transaction_id")
        finally:
            set("//sys/clusters", clusters)
            # TODO(babenko): wait for cluster sync
            time.sleep(2)

        assert read_table("//tmp/t2") == [{"a": i} for i in xrange(100)]

    @authors("ignat")
    def test_failed_cases(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        with pytest.raises(YtError):
            remote_copy(
                in_="//tmp/t1", out="//tmp/t2", spec={"cluster_name": "unexisting"}
            )

        with pytest.raises(YtError):
            remote_copy(
                in_="//tmp/t1",
                out="//tmp/t2",
                spec={
                    "cluster_name": self.REMOTE_CLUSTER_NAME,
                    "network_name": "unexisting",
                },
            )

        with pytest.raises(YtError):
            remote_copy(
                in_="//tmp/t1",
                out="//tmp/unexisting",
                spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
            )

        write_table("//tmp/t1", [{"a": "b"}, {"c": "d"}], driver=self.remote_driver)
        with pytest.raises(YtError):
            remote_copy(
                in_="//tmp/t1[:#1]",
                out="//tmp/unexisting",
                spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
            )

    @authors("asaitgalin", "ignat")
    def test_acl(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")

        create_user("u")
        create_user("u", driver=self.remote_driver)

        multicell_sleep()

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
            authenticated_user="u",
        )

        set(
            "//tmp/t1/@acl/end",
            make_ace("deny", "u", "read"),
            driver=self.remote_driver,
        )
        with pytest.raises(YtError):
            remote_copy(
                in_="//tmp/t1",
                out="//tmp/t2",
                spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
                authenticated_user="u",
            )
        set("//tmp/t1/@acl", [], driver=self.remote_driver)

        set(
            "//sys/schemas/transaction/@acl/end",
            make_ace("deny", "u", "create"),
            driver=self.remote_driver,
        )
        with pytest.raises(YtError):
            remote_copy(
                in_="//tmp/t1",
                out="//tmp/t2",
                spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
                authenticated_user="u",
            )

    @authors("asaitgalin", "ignat")
    def test_copy_attributes(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")
        create("table", "//tmp/t3")

        set("//tmp/t1/@custom_attr1", "attr_value1", driver=self.remote_driver)
        set("//tmp/t1/@custom_attr2", "attr_value2", driver=self.remote_driver)

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME, "copy_attributes": True},
        )

        assert get("//tmp/t2/@custom_attr1") == "attr_value1"
        assert get("//tmp/t2/@custom_attr2") == "attr_value2"

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t3",
            spec={
                "cluster_name": self.REMOTE_CLUSTER_NAME,
                "copy_attributes": True,
                "attribute_keys": ["custom_attr2"],
            },
        )

        assert not exists("//tmp/t3/@custom_attr1")
        assert get("//tmp/t3/@custom_attr2") == "attr_value2"

        with pytest.raises(YtError):
            remote_copy(
                in_=["//tmp/t1", "//tmp/t1"],
                out="//tmp/t2",
                spec={
                    "cluster_name": self.REMOTE_CLUSTER_NAME,
                    "copy_attributes": True,
                },
            )

    @authors("asaitgalin", "savrus")
    def test_copy_strict_schema(self):
        create(
            "table",
            "//tmp/t1",
            driver=self.remote_driver,
            attributes={
                "schema": make_schema(
                    [
                        {"name": "a", "type": "string", "sort_order": "ascending"},
                        {"name": "b", "type": "string"},
                    ],
                    unique_keys=True,
                )
            },
        )
        assert get("//tmp/t1/@schema_mode", driver=self.remote_driver) == "strong"

        create("table", "//tmp/t2")

        rows = [{"a": "x", "b": "v"}, {"a": "y", "b": "v"}]
        write_table("//tmp/t1", rows, driver=self.remote_driver)

        assert get("//tmp/t1/@schema_mode", driver=self.remote_driver) == "strong"

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == rows
        assert get("//tmp/t2/@schema/@strict")
        assert get("//tmp/t2/@schema/@unique_keys")
        assert get("//tmp/t2/@schema_mode") == "strong"

    @authors("gritukan")
    def test_legacy_controller_flag(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")
        op = remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert (
            get(op.get_path() + "/@progress/legacy_controller")
            == self.USE_LEGACY_CONTROLLERS
        )

    @authors("gritukan")
    @pytest.mark.timeout(300)
    def test_erasure_repair(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        create("table", "//tmp/t2")
        set("//tmp/t1/@erasure_codec", "reed_solomon_6_3", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        set(
            "//sys/@config/chunk_manager/enable_chunk_replicator",
            False,
            driver=self.remote_driver,
        )
        set("//sys/@config/chunk_manager/enable_chunk_replicator", False)
        multicell_sleep()

        chunk_id = get("//tmp/t1/@chunk_ids/0", driver=self.remote_driver)

        def run_operation():
            op = remote_copy(
                track=False,
                in_="//tmp/t1",
                out="//tmp/t2",
                spec={
                    "cluster_name": self.REMOTE_CLUSTER_NAME,
                    "max_failed_job_count": 1,
                    "delay_in_copy_chunk": 5000,
                    "erasure_chunk_repair_delay": 2000,
                    "repair_erasure_chunks": True,
                },
            )
            wait(lambda: len(op.get_running_jobs()) == 1)
            return op

        def check_everything():
            assert get("//tmp/t2/@chunk_count") == 1
            assert read_table("//tmp/t2") == [{"a": "b"}]

        def set_banned_flag_for_part_nodes(part_indicies, banned_flag):
            chunk_replicas = get(
                "#{}/@stored_replicas".format(chunk_id), driver=self.remote_driver
            )

            nodes_to_ban = []
            for part_index in part_indicies:
                nodes = list(
                    str(r)
                    for r in chunk_replicas
                    if r.attributes["index"] == part_index
                )
                nodes_to_ban += nodes

            set_banned_flag(banned_flag, nodes_to_ban, driver=self.remote_driver)

        def unban_all_nodes():
            nodes = list(get("//sys/cluster_nodes", driver=self.remote_driver).keys())
            set_banned_flag(False, nodes, driver=self.remote_driver)
            wait(
                lambda: get(
                    "#{}/@available".format(chunk_id), driver=self.remote_driver
                )
            )

        op = run_operation()
        # Some 3 parts are unavailable.
        # NB(gritukan): Cannot ban node before job started because CA will not start job until
        # all the parts were found.
        set_banned_flag_for_part_nodes([2, 3, 5], True)
        op.track()
        check_everything()
        unban_all_nodes()

        op = run_operation()
        # Some 4 parts are unavailable, repair is impossible.
        set_banned_flag_for_part_nodes([0, 1, 3, 8], True)
        time.sleep(8)
        # Job freezes.
        assert len(op.get_running_jobs()) == 1
        # Unban one part, job should complete.
        set_banned_flag_for_part_nodes([1], False)
        op.track()
        check_everything()
        unban_all_nodes()


##################################################################


class TestSchedulerRemoteCopyNetworks(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 9
    NUM_SCHEDULERS = 1

    NUM_REMOTE_CLUSTERS = 1

    NUM_MASTERS_REMOTE_0 = 1
    NUM_SCHEDULERS_REMOTE_0 = 0

    REMOTE_CLUSTER_NAME = "remote_0"

    @classmethod
    def setup_class(cls):
        super(TestSchedulerRemoteCopyNetworks, cls).setup_class()
        cls.remote_driver = get_driver(cluster=cls.REMOTE_CLUSTER_NAME)

    @classmethod
    def modify_node_config(cls, config):
        config["addresses"].append(
            ["custom_network", dict(config["addresses"])["default"]]
        )

    @authors("ignat")
    def test_default_network(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={"cluster_name": self.REMOTE_CLUSTER_NAME},
        )

        assert read_table("//tmp/t2") == [{"a": "b"}]

    @authors("ignat")
    def test_custom_network(self):
        create("table", "//tmp/t1", driver=self.remote_driver)
        write_table("//tmp/t1", {"a": "b"}, driver=self.remote_driver)

        create("table", "//tmp/t2")

        remote_copy(
            in_="//tmp/t1",
            out="//tmp/t2",
            spec={
                "cluster_name": self.REMOTE_CLUSTER_NAME,
                "network_name": "custom_network",
            },
        )

        assert read_table("//tmp/t2") == [{"a": "b"}]


##################################################################


class TestSchedulerRemoteCopyCommandsLegacy(TestSchedulerRemoteCopyCommands):
    USE_LEGACY_CONTROLLERS = 2


##################################################################


class TestSchedulerRemoteCopyCommandsMulticell(TestSchedulerRemoteCopyCommands):
    NUM_SECONDARY_MASTER_CELLS = 2
