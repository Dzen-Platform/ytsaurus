from yt.wrapper.client import Yt
from yt.wrapper.table import TablePath
import yt.wrapper.http as http
import yt.wrapper as yt

from helpers import TEST_DIR, get_temp_dsv_records

import pytest

import time
import tempfile
from copy import deepcopy

@pytest.mark.usefixtures("yt_env")
class TestClient(object):
    def setup(self):
        yt.create("user", attributes={"name": "tester"})
        yt.create("group", attributes={"name": "testers"})

    def teardown(self):
        yt.remove("//sys/users/tester", force=True)
        yt.remove("//sys/groups/testers", force=True)

    def test_client(self):
        client = Yt(config=yt.config.config)
        client.config["tabular_data_format"] = yt.format.DsvFormat()

        other_client = Yt(config=yt.config.config)
        other_client.config["proxy"]["force_ipv4"] = True
        other_client.config["tabular_data_format"] = yt.JsonFormat()

        deepcopy(client)

        old_proxy_url = yt.config["proxy"]["url"]
        try:
            yt.config["proxy"]["url"] = None
            if yt.config["api_version"] == "v2":
                assert client.get_user_name("") == None
            else:
                assert client.get_user_name("") == "root"

            client.set(TEST_DIR + "/node", "abc")
            assert client.get(TEST_DIR + "/node") == "abc"

            assert client.list(TEST_DIR) == ["node"]

            client.remove(TEST_DIR + "/node")
            assert not client.exists(TEST_DIR + "/node")

            client.mkdir(TEST_DIR + "/folder")
            assert client.get_type(TEST_DIR + "/folder") == "map_node"

            table = TEST_DIR + "/table"
            client.create("table", table)
            client.write_table(table, ["a=b\n"])
            assert "a=b\n" == client.read_table(table, raw=True).read()

            assert set(client.search(TEST_DIR)) == set([TEST_DIR, TEST_DIR + "/folder", table])

            other_table = TEST_DIR + "/other_table"
            client.copy(table, other_table)
            assert "a=b\n" == client.read_table(other_table, raw=True).read()
            client.move(table, TEST_DIR + "/moved_table")
            assert "a=b\n" == client.read_table(TEST_DIR + "/moved_table", raw=True).read()
            assert not client.exists(table)

            client.link(other_table, TEST_DIR + "/table_link")
            assert client.get_attribute(TEST_DIR + "/table_link&", "target_id") == \
                   client.get_attribute(other_table, "id")
            assert client.has_attribute(TEST_DIR + "/table_link&", "broken")

            client.set_attribute(other_table, "test_attr", "value")
            for attribute in ["id", "test_attr"]:
                assert attribute in client.list_attributes(other_table)

            assert not client.exists(client.find_free_subpath(TEST_DIR))

            assert client.check_permission("tester", "write", "//sys")["action"] == "deny"

            client.add_member("tester", "testers")
            assert client.get_attribute("//sys/groups/testers", "members") == ["tester"]
            client.remove_member("tester", "testers")
            assert client.get_attribute("//sys/groups/testers", "members") == []

            client.create_table(TEST_DIR + "/table")
            assert client.exists(TEST_DIR + "/table")

            temp_table = client.create_temp_table(TEST_DIR)
            assert client.get_type(temp_table) == "table"
            assert client.is_empty(temp_table)

            client.write_table(temp_table, get_temp_dsv_records())
            client.run_sort(temp_table, sort_by=["x"])
            assert client.is_sorted(temp_table)

            client.run_erase(TablePath(temp_table, start_index=0, end_index=5, client=client))
            assert client.row_count(temp_table) == 5

            client.run_map("cat", other_table, TEST_DIR + "/map_output")
            assert "a=b\n" == client.read_table(other_table, raw=True).read()

            client.write_table(TEST_DIR + "/first", ["x=1\n"])
            client.write_table(TEST_DIR + "/second", ["x=2\n"])
            client.run_merge([TEST_DIR + "/first", TEST_DIR + "/second"], TEST_DIR + "/merged_table")
            assert client.read_table(TEST_DIR + "/merged_table").read() == "x=1\nx=2\n"

            client.run_reduce("head -n 3", temp_table, TEST_DIR + "/reduce_output", reduce_by=["x"])
            assert client.row_count(TEST_DIR + "/reduce_output") == 3

            if yt.config["api_version"] != "v2":
                client.write_table("<sorted_by=[x]>" + TEST_DIR + "/first", ["x=1\n", "x=2\n"])
                client.write_table("<sorted_by=[x]>" + TEST_DIR + "/second", ["x=2\n", "x=3\n"])
                client.run_join_reduce("cat", [TEST_DIR + "/first", "<foreign=true>" + TEST_DIR + "/second"],
                    TEST_DIR + "/join_output", join_by=["x"])
                assert client.row_count(TEST_DIR + "/join_output") == 3

            mr_operation = client.run_map_reduce("cat", "head -n 3", temp_table, TEST_DIR + "/mapreduce_output",
                                                 reduce_by=["x"])
            assert client.get_operation_state(mr_operation.id) == "completed"
            assert client.row_count(TEST_DIR + "/mapreduce_output") == 3

            with client.Transaction():
                client.set("//@attr", 10)
                assert client.exists("//@attr")

            with client.PingableTransaction():
                client.set("//@other_attr", 10)
                assert client.exists("//@other_attr")

            tx = client.start_transaction(timeout=5000)
            with client.PingTransaction(tx, delay=1):
                assert client.exists("//sys/transactions/{0}".format(tx))
                client.TRANSACTION = tx
                assert client.lock(table) != "0-0-0-0"
                client.TRANSACTION = "0-0-0-0"

            client.ping_transaction(tx)
            client.abort_transaction(tx)
            with pytest.raises(yt.YtError):
                client.commit_transaction(tx)

            op = client.run_map("sleep 10; cat", temp_table, table, sync=False)
            assert not client.get_operation_state(op.id).is_unsuccessfully_finished()
            assert op.get_attributes()["state"] != "failed"
            time.sleep(0.5)
            client.suspend_operation(op.id)
            time.sleep(2.5)
            client.resume_operation(op.id)
            time.sleep(2.5)
            client.abort_operation(op.id)
            # Second abort on aborted operation should be silent
            client.abort_operation(op.id)
            assert op.get_progress()["total"] != 0
            assert op.get_stderrs() == []

            client.write_file(TEST_DIR + "/file", "0" * 1000)
            assert client.read_file(TEST_DIR + "/file").read() == "0" * 1000
            with pytest.raises(yt.YtError):
                client.smart_upload_file("/unexisting")

            assert other_client.get("/")
            assert '{"a":"b"}\n' == other_client.read_table(other_table, raw=True).read()

            with client.TempTable(TEST_DIR) as table:
                assert client.exists(table)
            assert not client.exists(table)

        finally:
            yt.config["proxy"]["url"] = old_proxy_url

    def test_default_api_version(self):
        client = Yt(proxy=yt.config["proxy"]["url"])
        client.get("/")
        assert client._api_version == "v2"

    def test_client_with_unknown_api_version(self):
        client = Yt(config=yt.config.config)
        client.config["api_version"] = None
        client.config["default_api_version_for_http"] = None
        if client.config["backend"] == "native":
            pytest.skip()
        client.get("/")
        assert client._api_version == "v3"

    def test_get_user_name(self):
        if yt.config["api_version"] == "v2":
            assert http.get_user_name("") == None
        else:
            # With disabled authentication in proxy it always return root
            assert http.get_user_name("") == "root"

        #assert http.get_user_name("") == None
        #assert http.get_user_name("12345") == None

        #token = "".join(["a"] * 16)
        #yt.set("//sys/tokens/" + token, "user")
        #assert http.get_user_name(token) == "user"

    def test_get_token(self):
        client = Yt(token="a" * 32)
        client.config["enable_token"] = True

        assert http.get_token(client) == "a" * 32

        _, filename = tempfile.mkstemp()
        with open(filename, "w") as fout:
            fout.write("b" * 32)
        client.config["token"] = None
        client.config["token_path"] = filename
        assert http.get_token(client) == "b" * 32
