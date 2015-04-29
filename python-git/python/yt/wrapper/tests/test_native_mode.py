#!/usr/bin/python

from yt.wrapper.client import Yt
from yt.wrapper.common import parse_bool
from yt.wrapper.tests.base import YtTestBase, TEST_DIR
from yt.wrapper.operation_commands import add_failed_operation_stderrs_to_error_message
from yt.environment import YTEnv
import yt.yson as yson
import yt.wrapper as yt

import inspect
import os
import time
import tempfile
import subprocess
import shutil

import pytest

def test_docs_exist():
    functions = inspect.getmembers(yt, lambda o: inspect.isfunction(o) and \
                                                 not o.__name__.startswith('_'))
    functions_without_doc = filter(lambda (name, func): not inspect.getdoc(func), functions)
    assert not functions_without_doc

    classes = inspect.getmembers(yt, lambda o: inspect.isclass(o))
    for name, cl  in classes:
        assert inspect.getdoc(cl)
        if name == "PingTransaction":
            continue # Python Thread is not documented O_o
        public_methods = inspect.getmembers(cl, lambda o: inspect.ismethod(o) and \
                                                          not o.__name__.startswith('_'))
        methods_without_doc = [method for name, method in public_methods
                                                            if (not inspect.getdoc(method))]
        assert not methods_without_doc

def test_reliable_remove_tempfiles():
    def dummy_buggy_upload(*args, **kwargs):
        raise TypeError

    def foo(rec):
        yield rec

    real_upload = yt.table_commands._prepare_binary.func_globals['_reliably_upload_files']
    yt.table_commands._prepare_binary.func_globals['_reliably_upload_files'] = dummy_buggy_upload
    old_tmp_dir = yt.config.LOCAL_TMP_DIR
    yt.config.LOCAL_TMP_DIR = tempfile.mkdtemp(dir=old_tmp_dir)
    try:
        files_before_fail = os.listdir(yt.config.LOCAL_TMP_DIR)
        with pytest.raises(TypeError):
            yt.table_commands._prepare_binary(foo, "mapper")
        files_after_fail = os.listdir(yt.config.LOCAL_TMP_DIR)
        assert files_after_fail == files_before_fail
    finally:
        yt.table_commands._prepare_binary.func_globals['_reliably_upload_files'] = real_upload
        shutil.rmtree(yt.config.LOCAL_TMP_DIR)
        yt.config.LOCAL_TMP_DIR = old_tmp_dir

class NativeModeTester(YtTestBase, YTEnv):
    @classmethod
    def setup_class(cls):
        super(NativeModeTester, cls).setup_class()
        # TODO(ignat): remove setting default format
        yt.config.format.TABULAR_DATA_FORMAT = yt.format.DsvFormat()

    @classmethod
    def teardown_class(cls):
        super(NativeModeTester, cls).teardown_class()

    # Check equality of records in dsv format
    def check(self, recordsA, recordsB):
        def prepare(records):
            return map(yt.loads_row, sorted(list(records)))
        self.assertEqual(prepare(recordsA), prepare(recordsB))


    def test_get_set_exists(self):
        self.assertTrue(yt.get("/"))
        self.assertTrue(len(yt.list("/")) > 1)
        self.assertRaises(yt.YtError, lambda: yt.get("//none"))

        self.assertTrue(yt.exists("/"))
        self.assertTrue(yt.exists(TEST_DIR))
        self.assertFalse(yt.exists(TEST_DIR + "/some_node"))

        self.assertRaises(yt.YtError, lambda: yt.set(TEST_DIR + "/some_node/embedded_node", {}))
        yt.set(TEST_DIR + "/some_node", {})

        self.assertTrue(yt.exists(TEST_DIR + "/some_node"))


    def test_remove(self):
        for recursive in [False, True]:
            with pytest.raises(yt.YtError):
                yt.remove(TEST_DIR + "/some_node", recursive=recursive)
            yt.remove(TEST_DIR + "/some_node", recursive=recursive, force=True)

        for force in [False, True]:
            yt.set(TEST_DIR + "/some_node", {})
            yt.remove(TEST_DIR + "/some_node",
                      recursive=True,
                      force=force)


    def test_mkdir(self):
        yt.mkdir(TEST_DIR, recursive=True)
        self.assertRaises(yt.YtError, lambda: yt.mkdir(TEST_DIR))


        self.assertRaises(yt.YtError, lambda: yt.mkdir(TEST_DIR + "/x/y"))
        yt.mkdir(TEST_DIR + "/x")
        yt.mkdir(TEST_DIR + "/x/y/z", recursive=True)


    def test_search(self):
        yt.mkdir(TEST_DIR + "/dir/other_dir", recursive=True)
        yt.create_table(TEST_DIR + "/dir/table")
        yt.upload_file("", TEST_DIR + "/file")

        self.assertEqual(set(yt.search(TEST_DIR)),
                         set([TEST_DIR, TEST_DIR + "/dir", TEST_DIR + "/dir/other_dir",
                              TEST_DIR + "/dir/table", TEST_DIR + "/file"]))

        self.assertEqual(set(yt.search(TEST_DIR, node_type="file")),
                         set([TEST_DIR + "/file"]))

        self.assertEqual(set(yt.search(TEST_DIR, node_type="table",
                                       path_filter=lambda x: x.find("dir") != -1)),
                         set([TEST_DIR + "/dir/table"]))

        # Search empty tables
        res = yt.search(TEST_DIR, attributes=["row_count"],
                        object_filter=lambda x: x.attributes.get("row_count", -1) == 0)
        self.assertEqual(sorted(list(res)),
                         sorted([yson.to_yson_type(TEST_DIR + "/dir/table", {"row_count": 0})]))

    def test_create(self):
        with pytest.raises(yt.YtError):
            yt.create("map_node", TEST_DIR + "/map", attributes={"type": "table"})

    def test_file_commands(self):
        self.assertRaises(yt.YtError, lambda: yt.upload_file("", TEST_DIR + "/dir/file"))

        file_path = TEST_DIR + "/file"
        yt.upload_file("", file_path)
        self.assertEqual("", yt.download_file(file_path).read())

        _, filename = tempfile.mkstemp()
        with open(filename, "w") as fout:
            fout.write("some content")

        destinationA = yt.smart_upload_file(filename, placement_strategy="hash")
        self.assertTrue(destinationA.startswith(yt.config.FILE_STORAGE))

        destinationB = yt.smart_upload_file(filename, placement_strategy="hash")
        self.assertEqual(destinationA, destinationB)

        destination = yt.smart_upload_file(filename, placement_strategy="random")
        path = os.path.join(os.path.basename(filename), yt.config.FILE_STORAGE)
        assert destination.startswith(path)

    def test_read_write(self):
        table = TEST_DIR + "/table"
        yt.create_table(table)
        self.check([], yt.read_table(table))

        yt.write_table(table, "x=1\n")
        self.check(["x=1\n"], yt.read_table(table))

        yt.write_table(table, ["x=1\n"])
        self.check(["x=1\n"], yt.read_table(table))

        yt.write_table(table, [{"x": 1}], raw=False)
        self.check(["x=1\n"], yt.read_table(table))

        yt.write_table(table, iter(["x=1\n"]))
        self.check(["x=1\n"], yt.read_table(table))

        yt.write_table(yt.TablePath(table, append=True), ["y=1\n"])
        self.check(["x=1\n", "y=1\n"], yt.read_table(table))

        yt.write_table(table, ["y=1\n"])
        self.check(["y=1\n"], yt.read_table(table))

        response_parameters = {}
        yt.read_table(table, response_parameters=response_parameters)
        assert {"start_row_index": 0, "approximate_row_count": 1} == response_parameters

        yt.write_table(table, [{"y": "1"}], raw=False)
        assert [{"y": "1"}] == list(yt.read_table(table, raw=False))

    def test_empty_table(self):
        dir = TEST_DIR + "/dir"
        table = dir + "/table"

        self.assertRaises(yt.YtError, lambda: yt.create_table(table))
        self.assertRaises(yt.YtError, lambda: yt.records_count(table))

        yt.create_table(table, recursive=True)
        self.assertEqual(0, yt.records_count(table))
        self.check([], yt.read_table(table, format=yt.DsvFormat()))

        yt.run_erase(table)
        self.assertEqual(0, yt.records_count(table))

        yt.remove(dir, recursive=True)
        self.assertRaises(yt.YtError, lambda: yt.create_table(table))

    def test_simple_copy_move(self):
        table = TEST_DIR + "/table"
        dir = TEST_DIR + "/dir"
        other_table = dir + "/other_table"
        yt.create_table(table)
        self.assertEqual([], list(yt.read_table(table)))

        self.assertRaises(yt.YtError, lambda: yt.copy(table, table))
        self.assertRaises(yt.YtError, lambda: yt.move(table, table))

        self.assertRaises(yt.YtError, lambda: yt.copy(table, other_table))
        self.assertRaises(yt.YtError, lambda: yt.move(table, other_table))

        yt.mkdir(dir)
        yt.copy(table, other_table)

        self.assertTrue(yt.exists(table))
        self.assertTrue(yt.exists(other_table))

        # Remove it after fixes in move
        yt.remove(other_table)

        yt.move(table, other_table)
        self.assertFalse(yt.exists(table))
        self.assertTrue(yt.exists(other_table))

    def test_merge(self):
        tableX = TEST_DIR + "/tableX"
        tableY = TEST_DIR + "/tableY"
        dir = TEST_DIR + "/dir"
        res_table = dir + "/other_table"

        yt.write_table(tableX, ["x=1\n"])
        yt.write_table(tableY, ["y=2\n"])

        self.assertRaises(yt.YtError, lambda: yt.run_merge([tableX, tableY], res_table))
        self.assertRaises(yt.YtError, lambda: yt.run_merge([tableX, tableY], res_table))

        yt.mkdir(dir)
        yt.run_merge([tableX, tableY], res_table)
        self.check(["x=1\n", "y=2\n"], yt.read_table(res_table))

        yt.run_merge(tableX, res_table)
        self.assertFalse(parse_bool(yt.get_attribute(res_table, "sorted")))
        self.check(["x=1\n"], yt.read_table(res_table))

        yt.run_sort(tableX, sort_by="x")
        yt.run_merge(tableX, res_table)
        self.assertTrue(parse_bool(yt.get_attribute(res_table, "sorted")))
        self.check(["x=1\n"], yt.read_table(res_table))

    def test_run_operation(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(table, ["x=1\n", "x=2\n"])

        yt.run_map("cat", table, table)
        self.check(["x=1\n", "x=2\n"], yt.read_table(table))

        yt.run_map("grep 2", table, other_table)
        self.check(["x=2\n"], yt.read_table(other_table))

        with pytest.raises(yt.YtError):
            yt.run_map("cat", [table, table + "xxx"], other_table)

    def test_sort(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(table, ["y=2\n", "x=1\n"])

        with pytest.raises(yt.YtError):
            yt.run_sort([table, other_table], other_table, sort_by=["y"])

        yt.run_sort(table, other_table, sort_by=["y"])
        self.assertItemsEqual(["x=1\n", "y=2\n"], yt.read_table(other_table))

        yt.run_sort(table, sort_by=["x"])
        self.assertItemsEqual(["y=2\n", "x=1\n"], yt.read_table(table))

    def test_write_many_chunks(self):
        yt.config.WRITE_BUFFER_SIZE = 1
        table = TEST_DIR + "/table"
        yt.write_table(table, ["x=1\n", "y=2\n", "z=3\n"])
        yt.write_table(table, ["x=1\n", "y=2\n", "z=3\n"])
        yt.write_table(table, ["x=1\n", "y=2\n", "z=3\n"])

    def test_python_operations(self):
        def change_x(rec):
            if "x" in rec:
                rec["x"] = int(rec["x"]) + 1
            yield rec

        def sum_y(key, recs):
            sum = 0
            for rec in recs:
                sum += int(rec.get("y", 1))
            yield {"x": key["x"], "y": sum}

        @yt.raw
        def change_field(line):
            yield "z=8\n"

        table = TEST_DIR + "/table"

        yt.write_table(table, ["x=1\n", "y=2\n"])
        yt.run_map(change_x, table, table, format=None)
        self.assertItemsEqual(["x=2\n", "y=2\n"], yt.read_table(table))

        yt.write_table(table, ["x=1\n", "y=2\n"])
        yt.run_map(change_x, table, table)
        self.assertItemsEqual(["x=2\n", "y=2\n"], yt.read_table(table))

        yt.write_table(table, ["x=1\n", "y=2\n"])
        yt.run_map(ChangeX__(), table, table)
        self.assertItemsEqual(["x=2\n", "y=2\n"], yt.read_table(table))

        yt.write_table(table, ["x=2\n", "x=2\ty=2\n"])
        yt.run_sort(table, sort_by=["x"])
        yt.run_reduce(sum_y, table, table, reduce_by=["x"])
        self.assertItemsEqual(["y=3\tx=2\n"], yt.read_table(table))

        yt.write_table(table, ["x=1\n", "y=2\n"])
        yt.run_map(change_field, table, table)
        self.assertItemsEqual(["z=8\n", "z=8\n"], yt.read_table(table))

    @add_failed_operation_stderrs_to_error_message
    def test_yamr_python_operations(self):
        def yamr_func(key, records):
            for rec in records:
                pass
            yield yt.Record("10", "20")

        table = TEST_DIR + "/table"
        output_table = TEST_DIR + "/output_table"
        yt.write_table(table, ["key=a\tvalue=b\n"])
        yt.run_map_reduce(mapper=None, reducer=yamr_func,
                          source_table=table, destination_table=output_table,
                          reduce_by="key", format=yt.YamrFormat())
        self.assertItemsEqual(["key=10\tvalue=20\n"], yt.read_table(output_table))

        with pytest.raises(yt.YtError):
            yt.run_map_reduce(mapper=None, reducer=yamr_func,
                              source_table=table, destination_table=output_table,
                              reduce_by="subkey", format=yt.YamrFormat())

    def test_binary_data_with_dsv(self):
        record = {"\tke\n\\\\y=": "\\x\\y\tz\n"}

        table = TEST_DIR + "/table"
        yt.write_table(table, map(yt.dumps_row, [record]))
        self.assertItemsEqual([record], map(yt.loads_row, yt.read_table(table)))

    def test_yt_binary(self):
        env = self.get_environment()
        if yt.config.VERSION == "v2":
            env["FALSE"] = '"false"'
            env["TRUE"] = '"true"'
        else:
            env["FALSE"] = '%false'
            env["TRUE"] = '%true'

        current_dir = os.path.dirname(os.path.abspath(__file__))
        proc = subprocess.Popen(
            os.path.join(current_dir, "../test_yt.sh"),
            shell=True,
            env=env)
        proc.communicate()
        self.assertEqual(proc.returncode, 0)


    def check_command(self, command, post_action=None, check_action=None, final_action=None):
        mutation_id = yt.common.generate_uuid()
        def run_command():
            yt.config.MUTATION_ID = mutation_id
            result = command()
            yt.config.MUTATION_ID = None
            return result

        result = run_command()
        if post_action is not None:
            post_action()
        for _ in xrange(5):
            yt.config.RETRY = True
            assert result == run_command()
            yt.config.RETRY = False
            if check_action is not None:
                assert check_action()

        if final_action is not None:
            final_action(result)

    def test_master_mutation_id(self):
        test_dir = os.path.join(TEST_DIR, "test")
        test_dir2 = os.path.join(TEST_DIR, "test2")
        test_dir3 = os.path.join(TEST_DIR, "test3")

        self.check_command(
            lambda: yt.set(test_dir, {"a": "b"}),
            lambda: yt.set(test_dir, {}),
            lambda: yt.get(test_dir) == {})

        self.check_command(
            lambda: yt.remove(test_dir3, force=True),
            lambda: yt.mkdir(test_dir3),
            lambda: yt.get(test_dir3) == {})

        parent_tx = yt.start_transaction()
        self.check_command(
            lambda: yt.start_transaction(parent_tx),
            None,
            lambda: len(yt.get("//sys/transactions/{0}/@nested_transaction_ids".format(parent_tx))) == 1)

        id = yt.start_transaction()
        self.check_command(lambda: yt.abort_transaction(id))

        id = yt.start_transaction()
        self.check_command(lambda: yt.commit_transaction(id))

        self.check_command(lambda: yt.move(test_dir, test_dir2))

    def test_scheduler_mutation_id(self):
        def abort(operation_id):
            yt.abort_operation(operation_id)
            time.sleep(1.0) # Wait for aborting transactions

        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(table, ["x=1\n", "x=2\n"])
        yt.create_table(other_table)

        for command, params in \
            [(
                "map",
                {"spec":
                    {"mapper":
                        {"command": "sleep 1; cat"},
                     "input_table_paths": [table],
                     "output_table_paths": [other_table]}})]:

            operations_count = yt.get("//sys/operations/@count")

            self.check_command(
                lambda: yson.loads(yt.driver.make_request(command, params)),
                None,
                lambda: yt.get("//sys/operations/@count") == operations_count + 1,
                abort)

    def test_lock(self):
        dir = TEST_DIR + "/dir"

        yt.mkdir(dir)
        self.assertEqual(0, len(yt.get(dir + "/@locks")))

        with yt.Transaction():
            yt.lock(dir)
            self.assertEqual(1, len(yt.get(dir + "/@locks")))

        self.assertEqual(0, len(yt.get(dir + "/@locks")))
        with yt.Transaction():
            assert yt.lock(dir, waitable=True) != "0-0-0-0"
            assert yt.lock(dir, waitable=True) == "0-0-0-0"
            assert yt.lock(dir, waitable=True, wait_for=1000) == "0-0-0-0"

        tx = yt.start_transaction()

        yt.config.TRANSACTION = tx
        yt.lock(dir, waitable=True)
        self.assertRaises(lambda: yt.lock(dir, waitable=True))
        yt.config.TRANSACTION = "0-0-0-0"

        with yt.Transaction():
            self.assertRaises(lambda: yt.lock(dir, waitable=True, wait_for=1000))

        yt.abort_transaction(tx)

    def test_start_row_index(self):
        table = TEST_DIR + "/table"

        yt.write_table(yt.TablePath(table, sorted_by=["a"]), ["a=b\n", "a=c\n", "a=d\n"])

        rsp = yt.read_table(table)._get_response()
        self.assertEqual(
            rsp.headers["X-YT-Response-Parameters"],
            {"start_row_index": 0,
             "approximate_row_count": 3})

        rsp = yt.read_table(yt.TablePath(table, start_index=1))._get_response()
        self.assertEqual(
            rsp.headers["X-YT-Response-Parameters"],
            {"start_row_index": 1,
             "approximate_row_count": 2})

        rsp = yt.read_table(yt.TablePath(table, lower_key=["d"]))._get_response()
        self.assertEqual(
            rsp.headers["X-YT-Response-Parameters"],
            {"start_row_index": 2,
             # When reading with key limits row count is estimated rounded up to the chunk row count.
             "approximate_row_count": 3})

        rsp = yt.read_table(yt.TablePath(table, lower_key=["x"]))._get_response()
        self.assertEqual(
            rsp.headers["X-YT-Response-Parameters"],
            {"start_row_index": 0,
             "approximate_row_count": 0})

    def test_read_with_retries(self):
        old_value = yt.config.RETRY_READ
        yt.config.RETRY_READ = True
        try:
            table = TEST_DIR + "/table"

            self.assertRaises(lambda: yt.read_table(table))

            yt.create_table(table)
            self.check([], list(yt.read_table(table, raw=False)))
            assert "" == yt.read_table(table).read()

            yt.write_table(table, ["x=1\n", "y=2\n"])
            self.check(["x=1\n", "y=2\n"], list(yt.read_table(table)))

            rsp = yt.read_table(table)
            self.check("x=1\n", rsp.next())
            self.assertRaises(lambda: yt.write_table(table, ["x=1\n", "y=2\n"]))
            rsp.close()

            self.assertEqual([{"x": "1"}, {"y": "2"}], list(yt.read_table(table, raw=False)))

            response_parameters = {}
            rsp = yt.read_table(table, response_parameters=response_parameters)
            assert {"start_row_index": 0, "approximate_row_count": 2} == response_parameters
            rsp.close()

        finally:
            yt.config.RETRY_READ = old_value

    def test_reduce_combiner(self):
        table = TEST_DIR + "/table"
        output_table = TEST_DIR + "/output_table"
        yt.write_table(table, ["x=1\n", "y=2\n"])

        yt.run_map_reduce(mapper=None, reduce_combiner="cat", reducer="cat", reduce_by=["x"],
                          source_table=table, destination_table=output_table)
        self.check(["x=1\n", "y=2\n"], sorted(list(yt.read_table(table))))

    def test_yamred_dsv(self):
        def foo(rec):
            yield rec

        table = TEST_DIR + "/table"
        yt.write_table(table, ["x=1\ty=2\n"])

        yt.run_map(foo, table, table,
                   input_format=yt.create_format("<key_column_names=[\"y\"]>yamred_dsv"),
                   output_format=yt.YamrFormat(has_subkey=False, lenval=False))
        self.check(["key=2\tvalue=x=1\n"], sorted(list(yt.read_table(table))))

    def test_schemaful_dsv(self):
        def foo(rec):
            yield rec

        table = TEST_DIR + "/table"
        yt.write_table(table, ["x=1\ty=2\n", "x=\\n\tz=3\n"])
        self.check(["1\n", "\\n\n"],
                   sorted(list(yt.read_table(table, format=yt.SchemafulDsvFormat(columns=["x"])))))

        yt.run_map(foo, table, table, format=yt.SchemafulDsvFormat(columns=["x"]))
        self.check(["x=1\n", "x=\\n\n"], sorted(list(yt.read_table(table))))

    def test_mount_unmount(self):
        if yt.config.VERSION == "v2":
            return

        table = TEST_DIR + "/table"
        yt.create_table(table)
        yt.set(table + "/@schema", [{"name": name, "type": "string"} for name in ["x", "y"]])
        yt.set(table + "/@key_columns", ["x"])

        tablet_id = yt.create("tablet_cell", attributes={"size": 1})
        while yt.get("//sys/tablet_cells/{0}/@health".format(tablet_id)) != 'good':
            time.sleep(0.1)

        yt.mount_table(table)
        while yt.get("{0}/@tablets/0/state".format(table)) != 'mounted':
            time.sleep(0.1)

        yt.unmount_table(table)
        while yt.get("{0}/@tablets/0/state".format(table)) != 'unmounted':
            time.sleep(0.1)

    @pytest.mark.skipif('os.environ.get("BUILD_ENABLE_LLVM", None) == "NO"')
    def test_select(self):
        if yt.config.VERSION == "v2":
            return

        table = TEST_DIR + "/table"

        def select():
            return list(yt.select_rows("* from [{0}]".format(table), format=yt.YsonFormat(format="text", process_table_index=False), raw=False))

        yt.remove(table, force=True)
        yt.create_table(table)
        yt.run_sort(table, sort_by=["x"])

        yt.set(table + "/@schema", [{"name": name, "type": "int64"} for name in ["x", "y", "z"]])
        yt.set(table + "/@key_columns", ["x"])

        assert [] == select()

        yt.write_table(yt.TablePath(table, append=True, sorted_by=["x"]),
                       ["{x=1;y=2;z=3}"], format=yt.YsonFormat())

        assert [{"x": 1, "y": 2, "z": 3}] == select()

    def test_insert_lookup_delete(self):
        if yt.config.VERSION == "v2":
            return

        yt.config.format.TABULAR_DATA_FORMAT = None

        # Name must differ with name of table in select test because of metadata caches
        table = TEST_DIR + "/table2"
        yt.remove(table, force=True)
        yt.create_table(table)
        yt.set(table + "/@schema", [{"name": name, "type": "string"} for name in ["x", "y"]])
        yt.set(table + "/@key_columns", ["x"])

        tablet_id = yt.create("tablet_cell", attributes={"size": 1})
        while yt.get("//sys/tablet_cells/{0}/@health".format(tablet_id)) != 'good':
            time.sleep(0.1)

        yt.mount_table(table)
        while yt.get("{0}/@tablets/0/state".format(table)) != 'mounted':
            time.sleep(0.1)

        yt.insert_rows(table, [{"x": "a", "y": "b"}])
        assert [{"x": "a", "y": "b"}] == list(yt.select_rows("* from [{0}]".format(table), raw=False))

        yt.insert_rows(table, [{"x": "c", "y": "d"}])
        assert [{"x": "c", "y": "d"}] == list(yt.lookup_rows(table, [{"x": "c"}]))

        yt.delete_rows(table, [{"x": "a"}])
        assert [{"x": "c", "y": "d"}] == list(yt.select_rows("* from [{0}]".format(table), raw=False))

        yt.config.format.TABULAR_DATA_FORMAT = yt.format.DsvFormat()

    def test_lenval_python_operations(self):
        def foo(rec):
            yield rec

        table = TEST_DIR + "/table"
        yt.write_table(table, ["key=1\tvalue=2\n"])
        yt.run_map(foo, table, table, format=yt.YamrFormat(lenval=True))
        self.check(["key=1\tvalue=2\n"], list(yt.read_table(table)))

    def test_wait_strategy_timeout(self):
        records = ["x=1\n", "y=2\n", "z=3\n"]
        pause = 3.0
        sleeep = "sleep {0}; cat > /dev/null".format(pause)
        desired_timeout = 1.0

        table = TEST_DIR + "/table"
        yt.write_table(table, records)

        # skip long loading time
        yt.run_map(sleeep, table, "//tmp/1", strategy=yt.WaitStrategy(), job_count=1)

        start = time.time()
        yt.run_map(sleeep, table, "//tmp/1", strategy=yt.WaitStrategy(), job_count=1)
        usual_time = time.time() - start
        loading_time = usual_time - pause

        start = time.time()
        with pytest.raises(yt.YtTimeoutError):
            yt.run_map(sleeep, table, "//tmp/1",
                       strategy=yt.WaitStrategy(timeout=desired_timeout), job_count=1)
        timeout_time = time.time() - start
        self.assertAlmostEqual(timeout_time, desired_timeout, delta=loading_time)

    def test_client(self):
        client = Yt(yt.config.http.PROXY)
        assert client.get("/")
        client.create("table", "//tmp/in")
        client.write_table("//tmp/in", ["a=b\n"])
        assert client.exists("//tmp/in")
        client.run_map("cat", "//tmp/in", "//tmp/out")
        assert client.exists("//tmp/out")
        with client.Transaction():
            yt.set("//@attr", 10)
            assert yt.exists("//@attr")

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
        yt.write_table(src_table_a, "1=a\t2=a\t3=a\n" * len_a, format=dsv)
        yt.write_table(src_table_b, "1=b\t2=b\t3=b\n" * len_b, format=dsv)

        assert yt.records_count(src_table_a) == len_a
        assert yt.records_count(src_table_b) == len_b

        def mix_table_indexes(row):
            row["_table_index_"] = row["TableIndex"]
            yield row
            row["_table_index_"] = 2
            yield row

        yt.table_commands.run_map(binary=mix_table_indexes,
                                  source_table=[src_table_a, src_table_b],
                                  destination_table=[dst_table_a, dst_table_b, dst_table_ab],
                                  input_format=dsv,
                                  output_format=schemaful_dsv)
        assert yt.records_count(dst_table_b) == len_b
        assert yt.records_count(dst_table_a) == len_a
        assert yt.records_count(dst_table_ab) == len_a + len_b
        for table in (dst_table_a, dst_table_b, dst_table_ab):
            row = yt.read_table(table, raw=False).next()
            for field in ("@table_index", "TableIndex", "_table_index_"):
                assert field not in row

    def test_attached_mode(self):
        table = TEST_DIR + "/table"

        yt.config.DETACHED = 0
        try:
            yt.write_table(table, ["x=1\n"])
            yt.run_map("cat", table, table)
            self.assertItemsEqual(["x=1\n"], yt.read_table(table))
            yt.run_merge(table, table)
            self.assertItemsEqual(["x=1\n"], yt.read_table(table))
        finally:
            yt.config.DETACHED = 1

    def test_abort_operation(self):
        table = TEST_DIR + "/table"
        op = yt.run_map("sleep 10; cat", table, table, sync=False)
        op.abort()
        self.assertEqual(op.get_state(), "aborted")


# Map method for test operations with python entities
class ChangeX__(object):
    def __call__(self, rec):
        if "x" in rec:
            rec["x"] = int(rec["x"]) + 1
        yield rec

class TestNativeModeV2(NativeModeTester):
    @classmethod
    def setup_class(cls):
        super(TestNativeModeV2, cls).setup_class()
        yt.config.VERSION = "v2"
        yt.config.COMMANDS = None

    @classmethod
    def teardown_class(cls):
        super(TestNativeModeV2, cls).teardown_class()

class TestNativeModeV3(NativeModeTester):
    @classmethod
    def setup_class(cls):
        super(TestNativeModeV3, cls).setup_class()
        yt.config.http.HEADER_FORMAT = "yson"
        yt.config.VERSION = "v3"
        yt.config.COMMANDS = None

    @classmethod
    def teardown_class(cls):
        super(TestNativeModeV3, cls).teardown_class()

