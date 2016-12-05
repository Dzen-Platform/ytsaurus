from __future__ import with_statement

from .helpers import TEST_DIR, check, set_config_option

import yt.wrapper.py_wrapper as py_wrapper
from yt.wrapper.table import TablePath, TempTable
from yt.wrapper.client import Yt
from yt.wrapper.common import parse_bool

import yt.zip as zip

from yt.packages.six.moves import xrange, map as imap

import yt.wrapper as yt

import os
import pytest
import tempfile
import shutil
import time
from io import BytesIO

@pytest.mark.usefixtures("yt_env")
class TestTableCommands(object):
    def _test_read_write(self):
        table = TEST_DIR + "/table"
        yt.create_table(table)
        check([], yt.read_table(table))

        yt.write_table(table, [{"x": 1}])
        check([{"x": 1}], yt.read_table(table))

        yt.write_table(table, [{"x": 1}])
        check([{"x": 1}], yt.read_table(table))

        yt.write_table(table, [{"x": 1}], raw=False)
        check([{"x": 1}], yt.read_table(table))

        yt.write_table(table, iter([{"x": 1}]))
        check([{"x": 1}], yt.read_table(table))

        yt.write_table(yt.TablePath(table, append=True), [{"y": 1}])
        check([{"x": 1}, {"y": 1}], yt.read_table(table))

        yt.write_table(yt.TablePath(table), [{"x": 1}, {"y": 1}])
        check([{"x": 1}, {"y": 1}], yt.read_table(table))

        yt.write_table(table, [{"y": 1}])
        check([{"y": 1}], yt.read_table(table))

        yt.write_table(table, BytesIO(b'{"y": 1}\n'), raw=True, format=yt.JsonFormat())
        check([{"y": 1}], yt.read_table(table))

        response_parameters = {}
        list(yt.read_table(table, response_parameters=response_parameters))
        assert {"start_row_index": 0, "approximate_row_count": 1} == response_parameters

        yt.write_table(table, [{"y": "1"}], raw=False)
        assert [{"y": "1"}] == list(yt.read_table(table, raw=False))

        with set_config_option("tabular_data_format", yt.DsvFormat()):
            yt.write_table(table, [b"x=1\n"], raw=True)

    def test_table_path(self, yt_env):
        path = yt.TablePath("//path/to/table", attributes={"my_attr": 10})
        assert path.attributes["my_attr"] == 10
        assert str(path) == "//path/to/table"

        with set_config_option("prefix", "//path/"):
            path = yt.TablePath("to/table", attributes={"my_attr": 10})
            assert path.attributes["my_attr"] == 10
            assert str(path) == "//path/to/table"

        if yt_env.version.startswith("18.5"):
            def mapper(rec):
                yield {"x": 1, "y": 2}

            table = TEST_DIR + "/table"
            yt.write_table(table, [{"key": "value"}])

            schema = [{"name": "x", "type": "int64"}, {"name": "y", "type": "int64"}]
            output_path = yt.TablePath(TEST_DIR + "/output", schema=schema)
            yt.run_map(mapper, table, output_path, format="yson")

            assert sorted([column["name"] for column in yt.get(TEST_DIR + "/output/@schema")]) == ["x", "y"]
            assert parse_bool(yt.get(TEST_DIR + "/output/@schema").attributes["strict"]) == True

    def test_read_write_with_retries(self):
        with set_config_option("write_retries/enable", True):
            self._test_read_write()

    def test_read_write_without_retries(self):
        with set_config_option("write_retries/enable", False):
            self._test_read_write()

    def test_empty_table(self):
        dir = TEST_DIR + "/dir"
        table = dir + "/table"

        with pytest.raises(yt.YtError):
            yt.create_table(table)
        with pytest.raises(yt.YtError):
            yt.row_count(table)

        yt.create_table(table, recursive=True)
        assert yt.row_count(table) == 0
        check([], yt.read_table(table, format=yt.DsvFormat()))

        yt.run_erase(table)
        assert yt.row_count(table) == 0

        yt.remove(dir, recursive=True)
        with pytest.raises(yt.YtError):
            yt.create_table(table)

    def test_create_temp_table(self):
        table = yt.create_temp_table(path=TEST_DIR)
        assert table.startswith(TEST_DIR)

        table = yt.create_temp_table(path=TEST_DIR, prefix="prefix")
        assert table.startswith(TEST_DIR + "/prefix")

        with TempTable() as table:
            assert yt.exists(table)
        assert not yt.exists(table)

    def test_write_many_chunks(self):
        with set_config_option("write_retries/chunk_size", 1):
            table = TEST_DIR + "/table"
            for i in xrange(3):
                yt.write_table("<append=%true>" + table, [{"x": 1}, {"y": 2}, {"z": 3}])
            assert yt.get(table + "/@chunk_count") == 9

    def test_binary_data_with_dsv(self):
        with set_config_option("tabular_data_format", yt.DsvFormat()):
            record = {"\tke\n\\\\y=": "\\x\\y\tz\n"}
            table = TEST_DIR + "/table"
            yt.write_table(table, [yt.dumps_row(record)], raw=True)
            assert [record] == list(yt.read_table(table))

    def test_mount_unmount(self, yt_env):
        table = TEST_DIR + "/table"
        yt.create_table(table, attributes={
            "dynamic": True,
            "schema": [
                {"name": "x", "type": "string", "sort_order": "ascending"},
                {"name": "y", "type": "string"}
            ]})

        tablet_id = yt.create("tablet_cell", attributes={"size": 1})
        while yt.get("//sys/tablet_cells/{0}/@health".format(tablet_id)) != 'good':
            time.sleep(0.1)

        yt.mount_table(table)
        while yt.get("{0}/@tablets/0/state".format(table)) != 'mounted':
            time.sleep(0.1)

        yt.unmount_table(table)
        while yt.get("{0}/@tablets/0/state".format(table)) != 'unmounted':
            time.sleep(0.1)

    @pytest.mark.xfail(run = False, reason = "In progress")
    def test_select(self):
        table = TEST_DIR + "/table"

        def select():
            schema = '<schema=[{"name"="x";"type"="int64"}; {"name"="y";"type"="int64"}; {"name"="z";"type"="int64"}]>'
            return list(yt.select_rows(
                '* from [{0}{1}]'.format(schema, table),
                format=yt.YsonFormat(format="text", process_table_index=False),
                raw=False))

        yt.remove(table, force=True)
        yt.create_table(table)
        yt.run_sort(table, sort_by=["x"])

        yt.set(table + "/@schema", [
            {"name": "x", "type": "int64", "sort_order": "ascending"},
            {"name": "y", "type": "int64"},
            {"name": "z", "type": "int64"}])

        assert [] == select()

        yt.write_table(yt.TablePath(table, append=True, sorted_by=["x"]),
                       ["{x=1;y=2;z=3}"], format=yt.YsonFormat())

        assert [{"x": 1, "y": 2, "z": 3}] == select()

    def test_insert_lookup_delete(self, yt_env):
        with set_config_option("tabular_data_format", None):
            # Name must differ with name of table in select test because of metadata caches
            table = TEST_DIR + "/table2"
            yt.create_table(table, attributes={
                "dynamic": True,
                "schema": [
                    {"name": "x", "type": "string", "sort_order": "ascending"},
                    {"name": "y", "type": "string"}
                ]})

            tablet_id = yt.create("tablet_cell", attributes={"size": 1})
            while yt.get("//sys/tablet_cells/{0}/@health".format(tablet_id)) != 'good':
                time.sleep(0.1)

            yt.mount_table(table)
            while yt.get("{0}/@tablets/0/state".format(table)) != 'mounted':
                time.sleep(0.1)

            yt.insert_rows(table, [{"x": "a", "y": "b"}], raw=False)
            assert [{"x": "a", "y": "b"}] == list(yt.select_rows("* from [{0}]".format(table), raw=False))

            yt.insert_rows(table, [{"x": "c", "y": "d"}], raw=False)
            assert [{"x": "c", "y": "d"}] == list(yt.lookup_rows(table, [{"x": "c"}], raw=False))

            yt.delete_rows(table, [{"x": "a"}], raw=False)
            assert [{"x": "c", "y": "d"}] == list(yt.select_rows("* from [{0}]".format(table), raw=False))

    def test_insert_lookup_delete_with_transaction(self, yt_env):
        if yt.config["backend"] != "native":
            pytest.skip()

        with set_config_option("tabular_data_format", None):
            # Name must differ with name of table in select test because of metadata caches
            table = TEST_DIR + "/table3"
            yt.remove(table, force=True)
            yt.create_table(table, attributes={
                "dynamic": True,
                "schema": [
                    {"name": "x", "type": "string", "sort_order": "ascending"},
                    {"name": "y", "type": "string"}
                ]})

            tablet_id = yt.create("tablet_cell", attributes={"size": 1})
            while yt.get("//sys/tablet_cells/{0}/@health".format(tablet_id)) != 'good':
                time.sleep(0.1)

            yt.mount_table(table)
            while yt.get("{0}/@tablets/0/state".format(table)) != 'mounted':
                time.sleep(0.1)

            vanilla_client = Yt(config=yt.config)

            assert list(vanilla_client.select_rows("* from [{0}]".format(table), raw=False)) == []
            assert list(vanilla_client.lookup_rows(table, [{"x": "a"}], raw=False)) == []

            with yt.Transaction(type="tablet", sticky=True):
                yt.insert_rows(table, [{"x": "a", "y": "a"}], raw=False)
                assert list(vanilla_client.select_rows("* from [{0}]".format(table), raw=False)) == []
                assert list(vanilla_client.lookup_rows(table, [{"x": "a"}], raw=False)) == []

            assert list(vanilla_client.select_rows("* from [{0}]".format(table), raw=False)) == [{"x": "a", "y": "a"}]
            assert list(vanilla_client.lookup_rows(table, [{"x": "a"}], raw=False)) == [{"x": "a", "y": "a"}]

            class FakeError(RuntimeError):
                pass

            with pytest.raises(FakeError):
                with yt.Transaction(type="tablet", sticky=True):
                    yt.insert_rows(table, [{"x": "b", "y": "b"}], raw=False)
                    raise FakeError()

            assert list(vanilla_client.select_rows("* from [{0}]".format(table), raw=False)) == [{"x": "a", "y": "a"}]
            assert list(vanilla_client.lookup_rows(table, [{"x": "a"}], raw=False)) == [{"x": "a", "y": "a"}]


    def test_start_row_index(self):
        table = TEST_DIR + "/table"

        yt.write_table(yt.TablePath(table, sorted_by=["a"]), [{"a": "b"}, {"a": "c"}, {"a": "d"}])

        with set_config_option("tabular_data_format", yt.JsonFormat()):
            rsp = yt.read_table(table, raw=True)
            assert rsp.response_parameters == {"start_row_index": 0,
                                               "approximate_row_count": 3}

            rsp = yt.read_table(yt.TablePath(table, start_index=1), raw=True)
            assert rsp.response_parameters == {"start_row_index": 1,
                                               "approximate_row_count": 2}

            rsp = yt.read_table(yt.TablePath(table, lower_key=["d"]), raw=True)
            assert rsp.response_parameters == \
                {"start_row_index": 2,
                 # When reading with key limits row count is estimated rounded up to the chunk row count.
                 "approximate_row_count": 3}

            rsp = yt.read_table(yt.TablePath(table, lower_key=["x"]), raw=True)
            assert rsp.response_parameters == {"approximate_row_count": 0}

    def test_table_index(self):
        dsv = yt.format.DsvFormat(enable_table_index=True, table_index_column="TableIndex")
        schemaful_dsv = yt.format.SchemafulDsvFormat(columns=['1', '2', '3'],
                                                     enable_table_index=True,
                                                     table_index_column="_table_index_")

        src_table_a = TEST_DIR + '/in_table_a'
        src_table_b = TEST_DIR + '/in_table_b'
        dst_table_a = TEST_DIR + '/out_table_a'
        dst_table_b = TEST_DIR + '/out_table_b'
        dst_table_ab = TEST_DIR + '/out_table_ab'

        len_a = 5
        len_b = 3

        yt.create_table(src_table_a, recursive=True, ignore_existing=True)
        yt.create_table(src_table_b, recursive=True, ignore_existing=True)
        yt.write_table(src_table_a, b"1=a\t2=a\t3=a\n" * len_a, format=dsv, raw=True)
        yt.write_table(src_table_b, b"1=b\t2=b\t3=b\n" * len_b, format=dsv, raw=True)

        assert yt.row_count(src_table_a) == len_a
        assert yt.row_count(src_table_b) == len_b

        def mix_table_indexes(row):
            row["_table_index_"] = row["TableIndex"]
            yield row
            row["_table_index_"] = 2
            yield row

        yt.run_operation_commands.run_map(binary=mix_table_indexes,
                                          source_table=[src_table_a, src_table_b],
                                          destination_table=[dst_table_a, dst_table_b, dst_table_ab],
                                          input_format=dsv,
                                          output_format=schemaful_dsv)
        assert yt.row_count(dst_table_b) == len_b
        assert yt.row_count(dst_table_a) == len_a
        assert yt.row_count(dst_table_ab) == len_a + len_b
        for table in (dst_table_a, dst_table_b, dst_table_ab):
            rsp = yt.read_table(table, raw=False)
            row = next(rsp)
            for field in ("@table_index", "TableIndex", "_table_index_"):
                assert field not in row

    def test_erase(self):
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"a": i} for i in xrange(10)])
        assert yt.row_count(table) == 10
        yt.run_erase(TablePath(table, start_index=0, end_index=5))
        assert yt.row_count(table) == 5
        yt.run_erase(TablePath(table, start_index=0, end_index=5))
        assert yt.row_count(table) == 0

    def test_read_with_table_path(self, yt_env):
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"y": "w3"}, {"x": "b", "y": "w1"}, {"x": "a", "y": "w2"}])
        yt.run_sort(table, sort_by=["x", "y"])

        def read_table(**kwargs):
            kwargs["name"] = kwargs.get("name", table)
            return list(yt.read_table(TablePath(**kwargs), raw=False))

        assert read_table(lower_key="a", upper_key="d") == [{"x": "a", "y": "w2"},
                                                            {"x": "b", "y": "w1"}]
        assert read_table(columns=["y"]) == [{"y": "w" + str(i)} for i in [3, 2, 1]]
        assert read_table(lower_key="a", end_index=2, columns=["x"]) == [{"x": "a"}]
        assert read_table(start_index=0, upper_key="b") == [{"x": None, "y": "w3"}, {"x": "a", "y": "w2"}]
        assert read_table(start_index=1, columns=["x"]) == [{"x": "a"}, {"x": "b"}]
        assert read_table(ranges=[{"lower_limit": {"row_index": 1}}], columns=["x"]) == [{"x": "a"}, {"x": "b"}]

        assert read_table(name=table + "{y}[:#2]") == [{"y": "w3"}, {"y": "w2"}]
        assert read_table(name=table + "[#1:]") == [{"x": "a", "y": "w2"}, {"x": "b", "y": "w1"}]

        assert read_table(name=
                          "<ranges=[{"
                          "lower_limit={key=[b]}"
                          "}]>" + table) == [{"x": "b", "y": "w1"}]
        assert read_table(name=
                          "<ranges=[{"
                          "upper_limit={row_index=2}"
                          "}]>" + table) == [{"x": None, "y": "w3"}, {"x": "a", "y": "w2"}]

        with pytest.raises(yt.YtError):
            assert read_table(ranges=[{"lower_limit": {"index": 1}}], end_index=1)
        with pytest.raises(yt.YtError):
            read_table(name=TablePath(table, lower_key="a", start_index=1))
        with pytest.raises(yt.YtError):
            read_table(name=TablePath(table, upper_key="c", end_index=1))

        table_path = TablePath(table, exact_index=1)
        assert list(yt.read_table(table_path.to_yson_string(), format=yt.DsvFormat())) == [{"x": "a", "y": "w2"}]

        yt.write_table(table, [{"x": "b"}, {"x": "a"}, {"x": "c"}])
        with pytest.raises(yt.YtError):
            yt.read_table(TablePath(table, lower_key="a"))
        # No prefix
        with pytest.raises(yt.YtError):
            TablePath("abc")
        # Prefix should start with //
        yt.config["prefix"] = "abc/"
        with pytest.raises(yt.YtError):
            TablePath("abc")
        # Prefix should end with /
        yt.config["prefix"] = "//abc"
        with pytest.raises(yt.YtError):
            TablePath("abc")

    def test_huge_table(self):
        table = TEST_DIR + "/table"
        power = 3
        format = yt.JsonFormat()
        records = imap(format.dumps_row, ({"k": i, "s": i * i, "v": "long long string with strange symbols"
                                                                    " #*@*&^$#%@(#!@:L|L|KL..,,.~`"}
                       for i in xrange(10 ** power)))
        yt.write_table(table, yt.StringIterIO(records), format=format, raw=True)

        assert yt.row_count(table) == 10 ** power

        row_count = 0
        for _ in yt.read_table(table):
            row_count += 1
        assert row_count == 10 ** power

    def test_remove_locks(self):
        from yt.wrapper.table_helpers import _remove_locks
        table = TEST_DIR + "/table"
        yt.create_table(table)
        try:
            for _ in xrange(5):
                tx = yt.start_transaction(timeout=10000)
                yt.config.TRANSACTION = tx
                yt.lock(table, mode="shared")
            yt.config.TRANSACTION = "0-0-0-0"
            assert len(yt.get_attribute(table, "locks")) == 5
            _remove_locks(table)
            assert yt.get_attribute(table, "locks") == []

            tx = yt.start_transaction(timeout=10000)
            yt.config.TRANSACTION = tx
            yt.lock(table)
            yt.config.TRANSACTION = "0-0-0-0"
            _remove_locks(table)
            assert yt.get_attribute(table, "locks") == []
        finally:
            yt.config.TRANSACTION = "0-0-0-0"

    def _set_banned(self, value):
        # NB: we cannot unban proxy using proxy, so we must using client for that.
        client = Yt(config={"backend": "native", "driver_config": yt.config["driver_config"]})
        proxy = "//sys/proxies/" + client.list("//sys/proxies")[0]
        client.set(proxy + "/@banned".format(proxy), value)
        time.sleep(1)

    def test_banned_proxy(self):
        if yt.config["backend"] == "native":
            pytest.skip()

        table = TEST_DIR + "/table"
        yt.create_table(table)

        self._set_banned("true")
        with set_config_option("proxy/request_retry_count", 1,
                               final_action=lambda: self._set_banned("false")):
            with pytest.raises(yt.YtProxyUnavailable):
                yt.get(table)

            try:
                yt.get(table)
            except yt.YtProxyUnavailable as err:
                assert "banned" in str(err)

    def test_error_occured_after_starting_to_write_chunked_requests(self):
        if yt.config["api_version"] != "v3":
            pytest.skip()

        with set_config_option("proxy/content_encoding", "identity"):
            table = TEST_DIR + "/table"
            try:
                yt.write_table(table, iter([b'{"abc": "123"}\n'] * 100000 + [b"{a:b}"] + [b'{"abc": "123"}\n'] * 100000), raw=True, format=yt.JsonFormat())
            except yt.YtResponseError as err:
                assert "JSON" in str(err), "Incorrect error message: " + str(err)
            else:
                assert False, "Failed to catch response error"

    def test_reliable_remove_tempfiles_in_py_wrap(self):
        def foo(rec):
            yield rec

        def uploader(files):
            assert files != []
            assert os.listdir(yt.config["local_temp_directory"]) != []

        new_temp_dir = tempfile.mkdtemp(dir=yt.config["local_temp_directory"])
        with set_config_option("local_temp_directory", new_temp_dir,
                               final_action=lambda: shutil.rmtree(new_temp_dir)):
            assert os.listdir(yt.config["local_temp_directory"]) == []
            with pytest.raises(Exception):
                py_wrapper.wrap(function=foo, operation_type="mapper", uploader=None,
                                client=None, input_format=None, output_format=None, group_by=None)
            assert os.listdir(yt.config["local_temp_directory"]) == []
            py_wrapper.wrap(function=foo, operation_type="mapper", uploader=uploader,
                            client=None, input_format=None, output_format=None, group_by=None)
            assert os.listdir(yt.config["local_temp_directory"]) == []

    def test_write_compressed_table_data(self):
        fd, filename = tempfile.mkstemp()
        os.close(fd)

        with zip.GzipFile(filename, "w", 5) as fout:
            fout.write(b"x=1\nx=2\nx=3\n")

        with open(filename, "rb") as f:
            with set_config_option("proxy/content_encoding", "gzip"):
                if yt.config["backend"] == "native":
                    with pytest.raises(yt.YtError):  # not supported for native backend
                        yt.write_table(TEST_DIR + "/table", f, format="dsv", is_stream_compressed=True, raw=True)
                else:
                    yt.write_table(TEST_DIR + "/table", f, format="dsv", is_stream_compressed=True, raw=True)
                    check([{"x": "1"}, {"x": "2"}, {"x": "3"}], yt.read_table(TEST_DIR + "/table"))
