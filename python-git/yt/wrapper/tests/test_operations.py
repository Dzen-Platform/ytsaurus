from __future__ import print_function

from .helpers import TEST_DIR, PYTHONPATH, get_test_file_path, check, set_config_option, \
                     build_python_egg, TESTS_SANDBOX, run_python_script_with_check, \
                     ENABLE_JOB_CONTROL, dumps_yt_config

from yt.wrapper.py_wrapper import create_modules_archive_default, TempfilesManager
from yt.common import which, makedirp
from yt.wrapper.common import parse_bool
from yt.wrapper.operation_commands import add_failed_operation_stderrs_to_error_message
from yt.wrapper.table import TablePath
import yt.logger as logger
import yt.subprocess_wrapper as subprocess

from yt.packages.six import b
from yt.packages.six.moves import xrange, zip as izip

import yt.wrapper as yt

import os
import imp
import sys
import time
import string
import tempfile
import random
import logging
import pytest

class AggregateMapper(object):
    def __init__(self):
        self.sum = 0

    def __call__(self, row):
        self.sum += int(row["x"])

    def finish(self):
        yield {"sum": self.sum}

class AggregateReducer(object):
    def __init__(self):
        self.sum = 0

    def start(self):
        for i in [1, 2]:
            yield {"sum": i}

    def __call__(self, key, rows):
        for row in rows:
            self.sum += int(row["y"])

    def finish(self):
        yield {"sum": self.sum}

class CreateModulesArchive(object):
    def __call__(self, tempfiles_manager=None, custom_python_used=False):
        return create_modules_archive_default(tempfiles_manager, custom_python_used, None)


@pytest.mark.usefixtures("yt_env")
class TestOperations(object):
    def setup(self):
        yt.config["tabular_data_format"] = yt.format.JsonFormat()

    def teardown(self):
        yt.config["tabular_data_format"] = None
        yt.remove("//tmp/yt_wrapper/file_storage", recursive=True, force=True)

    def random_string(self, length):
        char_set = string.ascii_lowercase + string.digits + string.ascii_uppercase
        return "".join(random.sample(char_set, length))

    def test_merge(self):
        tableX = TEST_DIR + "/tableX"
        tableY = TEST_DIR + "/tableY"
        dir = TEST_DIR + "/dir"
        res_table = dir + "/other_table"

        yt.write_table(tableX, [{"x": 1}])
        yt.write_table(tableY, [{"y": 2}])

        with pytest.raises(yt.YtError):
            yt.run_merge([tableX, tableY], res_table)
        with pytest.raises(yt.YtError):
            yt.run_merge([tableX, tableY], res_table)

        yt.mkdir(dir)
        yt.run_merge([tableX, tableY], res_table)
        check([{"x": 1}, {"y": 2}], yt.read_table(res_table), ordered=False)

        yt.run_merge(tableX, res_table)
        assert not parse_bool(yt.get_attribute(res_table, "sorted"))
        check([{"x": 1}], yt.read_table(res_table))

        yt.run_sort(tableX, sort_by="x")
        yt.run_merge(tableX, res_table)
        assert parse_bool(yt.get_attribute(res_table, "sorted"))
        check([{"x": 1}], yt.read_table(res_table))

        # XXX(asaitgalin): Uncomment when st/YT-5770 is done.
        # yt.run_merge(yt.TablePath(tableX, columns=["y"]), res_table)
        # assert not parse_bool(yt.get_attribute(res_table, "sorted"))
        # check([{}], yt.read_table(res_table))

    def test_auto_merge(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(table, [{"x": i} for i in xrange(6)])

        old_auto_merge_output = yt.config["auto_merge_output"]

        yt.config["auto_merge_output"]["min_chunk_count"] = 2
        yt.config["auto_merge_output"]["max_chunk_size"] = 5 * 1024
        try:
            yt.config["auto_merge_output"]["action"] = "none"
            yt.run_map("cat", table, other_table, job_count=6)
            assert yt.get_attribute(other_table, "chunk_count") == 6
            yt.config["auto_merge_output"]["action"] = "merge"
            yt.run_map("cat", table, other_table, job_count=6)
            assert yt.get_attribute(other_table, "chunk_count") == 1
        finally:
            yt.config["auto_merge_output"].update(old_auto_merge_output)

    def test_sort(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"

        columns = [(self.random_string(7), self.random_string(7)) for _ in xrange(10)]
        yt.write_table(table, [b("x={0}\ty={1}\n".format(*c)) for c in columns], format=yt.DsvFormat(), raw=True)

        with pytest.raises(yt.YtError):
            yt.run_sort([table, other_table], other_table, sort_by=["y"])

        yt.run_sort(table, other_table, sort_by=["x"])
        assert [{"x": x, "y": y} for x, y in sorted(columns, key=lambda c: c[0])] == list(yt.read_table(other_table))

        yt.run_sort(table, sort_by=["x"])
        assert list(yt.read_table(table)) == list(yt.read_table(other_table))

        # Sort again and check that everything is ok
        yt.run_sort(table, sort_by=["x"])
        assert list(yt.read_table(table)) == list(yt.read_table(other_table))

        yt.run_sort(table, sort_by=["y"])
        assert [{"x": x, "y": y} for x, y in sorted(columns, key=lambda c: c[1])] == list(yt.read_table(table))

        assert yt.is_sorted(table)

        with pytest.raises(yt.YtError):
            yt.run_sort(table, sort_by=None)

    def test_run_operation(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(table, [{"x": 1}, {"x": 2}])

        yt.run_map("cat", table, table)
        check([{"x": 1}, {"x": 2}], list(yt.read_table(table)), ordered=False)
        yt.run_sort(table, sort_by=["x"])
        with pytest.raises(yt.YtError):
            yt.run_reduce("cat", table, [], reduce_by=["x"])

        yt.run_reduce("cat", table, table, reduce_by=["x"])
        check([{"x": 1}, {"x": 2}], yt.read_table(table))

        with pytest.raises(yt.YtError):
            yt.run_map("cat", table, table, table_writer={"max_row_weight": 1})

        yt.run_map("grep 2", table, other_table)
        check([{"x": 2}], yt.read_table(other_table))

        with pytest.raises(yt.YtError):
            yt.run_map("cat", [table, table + "xxx"], other_table)

        with pytest.raises(yt.YtError):
            yt.run_reduce("cat", table, other_table, reduce_by=None)

        # Run reduce on unsorted table
        with pytest.raises(yt.YtError):
            yt.run_reduce("cat", other_table, table, reduce_by=["x"])

        yt.write_table(table,
                       [
                           {"a": 12,  "b": "ignat"},
                                     {"b": "max"},
                           {"a": "x", "b": "name", "c": 0.5}
                       ])
        yt.run_map("PYTHONPATH=. ./capitalize_b.py",
                   TablePath(table, columns=["b"]), other_table,
                   files=get_test_file_path("capitalize_b.py"),
                   format=yt.DsvFormat())
        records = yt.read_table(other_table, raw=False)
        assert sorted([rec["b"] for rec in records]) == ["IGNAT", "MAX", "NAME"]
        assert sorted([rec["c"] for rec in records]) == []

        with pytest.raises(yt.YtError):
            yt.run_map("cat", table, table, local_files=get_test_file_path("capitalize_b.py"),
                                            files=get_test_file_path("capitalize_b.py"))


    def test_stderr_table(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(TablePath(table, sorted_by=["x"]), [{"x": 1}, {"x": 2}])

        stderr_table = TEST_DIR + "/stderr_table"

        yt.run_map("echo map >&2 ; cat", table, other_table, stderr_table=stderr_table)
        row_list = list(yt.read_table(stderr_table, raw=False))
        assert len(row_list) > 0
        for r in row_list:
            assert r["data"] == "map\n"

        yt.run_reduce("echo reduce >&2 ; cat",
                      table, other_table, stderr_table=stderr_table,
                      reduce_by=["x"])
        row_list = list(yt.read_table(stderr_table, raw=False))
        assert len(row_list) > 0
        for r in row_list:
            assert r["data"] == "reduce\n"

        yt.run_map_reduce(
            "echo mr-map >&2 ; cat",
            "echo mr-reduce >&2 ; cat",
            table, other_table,
            stderr_table=stderr_table, reduce_by=["x"])

        row_list = list(yt.read_table(stderr_table, raw=False))
        assert len(row_list) > 0
        for r in row_list:
            assert r["data"] in ["mr-map\n", "mr-reduce\n"]

    def test_stderr_table_inside_transaction(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(TablePath(table, sorted_by=["x"]), [{"x": 1}, {"x": 2}])

        stderr_table = TEST_DIR + "/stderr_table"
        try:
            with yt.Transaction():
                yt.run_map("echo map >&2 ; cat", table, other_table, stderr_table=stderr_table)
                raise RuntimeError
        except RuntimeError:
            pass

        assert not yt.exists(other_table)

        # We expect stderr to be saved nevertheless.
        row_list = list(yt.read_table(stderr_table, raw=False))
        assert len(row_list) > 0
        for r in row_list:
            assert r["data"] == "map\n"


    def test_run_standalone_binary(self):
        if sys.version_info[0] >= 3:
            pytest.skip()

        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(table, [{"x": 1}, {"x": 2}])

        binary = get_test_file_path("standalone_binary.py")

        env = {
            "YT_CONFIG_PATCHES": dumps_yt_config(),
            "PYTHONPATH": PYTHONPATH
        }
        subprocess.check_call(["python", binary, table, other_table], env=env, stderr=sys.stderr)
        check([{"x": 1}, {"x": 2}], yt.read_table(other_table))

    @add_failed_operation_stderrs_to_error_message
    def test_run_join_operation(self, yt_env):
        table1 = TEST_DIR + "/first"
        yt.write_table("<sorted_by=[x]>" + table1, [{"x": 1}])
        table2 = TEST_DIR + "/second"
        yt.write_table("<sorted_by=[x]>" + table2, [{"x": 2}])
        unsorted_table = TEST_DIR + "/unsorted_table"
        yt.write_table(unsorted_table, [{"x": 3}])
        table = TEST_DIR + "/table"

        yt.run_join_reduce("cat", ["<primary=true>" + table1, table2], table, join_by=["x"])
        check([{"x": 1}], yt.read_table(table))

        # Run join-reduce without join_by
        with pytest.raises(yt.YtError):
            yt.run_join_reduce("cat", ["<primary=true>" + table1, table2], table)

        # Run join-reduce on unsorted table
        with pytest.raises(yt.YtError):
            yt.run_join_reduce("cat", ["<primary=true>" + unsorted_table, table2], table, join_by=["x"])

        yt.run_join_reduce("cat", [table1, "<foreign=true>" + table2], table, join_by=["x"])
        check([{"x": 1}], yt.read_table(table))

        # Run join-reduce without join_by
        with pytest.raises(yt.YtError):
            yt.run_join_reduce("cat", [table1, "<foreign=true>" + table2], table)

        # Run join-reduce on unsorted table
        with pytest.raises(yt.YtError):
            yt.run_join_reduce("cat", [unsorted_table, "<foreign=true>" + table2], table, join_by=["x"])

        yt.write_table("<sorted_by=[x;y]>" + table1, [{"x": 1, "y": 1}])
        yt.write_table("<sorted_by=[x]>" + table2, [{"x": 1}])

        def func(key, rows):
            assert list(key) == ["x"]
            for row in rows:
                yield row

        yt.run_reduce(func, [table1, "<foreign=true>" + table2], table,
                      reduce_by=["x","y"], join_by=["x"],
                      format=yt.YsonFormat(process_table_index=None))
        check([{"x": 1, "y": 1}, {"x": 1}], yt.read_table(table))

        # Reduce with join_by, but without foreign tables
        with pytest.raises(yt.YtError):
            yt.run_reduce("cat", [table1, table2], table, join_by=["x"])

    @add_failed_operation_stderrs_to_error_message
    def test_python_operations(self, yt_env):
        test_script = """\
from __future__ import print_function
import yt.wrapper as yt

import yt.yson as yson
import sys

{mapper_code}

if __name__ == "__main__":
    stdin = sys.stdin
    if sys.version_info[0] >= 3:
        stdin = sys.stdin.buffer

    yt.update_config(yson.load(stdin, always_create_attributes=False))
    yt.config["pickling"]["enable_tmpfs_archive"] = False
    print(yt.run_map({mapper}, "{source_table}", "{destination_table}", format="json").id)
"""

        methods_pickling_test = ("""\
class C(object):
    {decorator}
    def do({self}):
        return 0

    def __call__(self, rec):
        self.do()
        yield rec
""", 'C()')

        metaclass_pickling_test = ("""\
from yt.packages.six import add_metaclass
from abc import ABCMeta, abstractmethod

@add_metaclass(ABCMeta)
class AbstractClass(object):
    @abstractmethod
    def __init__(self):
        pass

class DoSomething(AbstractClass):
    def __init__(self):
        pass

    def do_something(self, rec):
        if "x" in rec:
            rec["x"] = int(rec["x"]) + 1
        return rec

class MapperWithMetaclass(object):
    def __init__(self):
        self.some_external_code = DoSomething()

    def map(self, rec):
        yield self.some_external_code.do_something(rec)
""", 'MapperWithMetaclass().map')

        simple_pickling_test = ("""\
def mapper(rec):
    yield rec
""", 'mapper')

        def _format_script(script, **kwargs):
            kwargs.update(dict(zip(("mapper_code", "mapper"), script)))
            return test_script.format(**kwargs)

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
            yield b"z=8\n"

        @yt.aggregator
        def sum_x(recs):
            sum = 0
            for rec in recs:
                sum += int(rec.get("x", 0))
            yield {"sum": sum}

        @yt.raw_io
        def sum_x_raw():
            sum = 0
            for line in sys.stdin:
                x = line.strip().split("=")[1]
                sum += int(x)
            sys.stdout.write("sum={0}\n".format(sum))

        def write_statistics(row):
            yt.write_statistics({"row_count": 1})
            yt.get_blkio_cgroup_statistics()
            yt.get_memory_cgroup_statistics()
            yield row

        table = TEST_DIR + "/table"

        yt.write_table(table, [{"x": 1}, {"y": 2}])
        yt.run_map(change_x, table, table, format=None, spec={"enable_core_dump": True})
        check(yt.read_table(table), [{"x": 2}, {"y": 2}], ordered=False)

        yt.write_table(table, [{"x": 1}, {"y": 2}])
        yt.run_map(change_x, table, table)
        check(yt.read_table(table),  [{"x": 2}, {"y": 2}])

        yt.write_table(table, [{"x": 1}, {"x": 2}, {"x": 3}])
        run_python_script_with_check(
            yt_env,
            _format_script(simple_pickling_test, source_table=table, destination_table=table))
        check(yt.read_table(table), [{"x": 1}, {"x": 2}, {"x": 3}])

        for decorator, self_ in [("", "self"), ("@staticmethod", ""), ("@classmethod", "cls")]:
            yt.write_table(table, [{"x": 1}, {"y": 2}])
            script = (methods_pickling_test[0].format(decorator=decorator, self=self_),
                      methods_pickling_test[1])
            run_python_script_with_check(
                yt_env,
                _format_script(script, source_table=table, destination_table=table))
            check(yt.read_table(table), [{"x": 1}, {"y": 2}], ordered=False)

        yt.write_table(table, [{"x": 1}, {"y": 2}])
        run_python_script_with_check(
            yt_env,
            _format_script(metaclass_pickling_test, source_table=table, destination_table=table))
        check(yt.read_table(table), [{"x": 2}, {"y": 2}], ordered=False)

        yt.write_table(table, [{"x": 2}, {"x": 2, "y": 2}])
        yt.run_sort(table, sort_by=["x"])
        yt.run_reduce(sum_y, table, table, reduce_by=["x"])
        check(yt.read_table(table), [{"y": 3, "x": 2}], ordered=False)

        yt.write_table(table, [{"x": "1"}, {"y": "2"}])
        yt.run_map(change_field, table, table, format=yt.DsvFormat())
        check(yt.read_table(table), [{"z": "8"}, {"z": "8"}])

        yt.write_table(table, [{"x": 1}, {"x": 2}, {"x": 3}])
        yt.run_map(sum_x, table, table)
        check(yt.read_table(table), [{"sum": 6}])

        yt.write_table(table, [{"x": "3"}] * 3)
        yt.run_map(sum_x_raw, table, table, format=yt.DsvFormat())
        check(yt.read_table(table), [{"sum": "9"}])

        yt.write_table(table, [{"x": 1}, {"y": 2}])
        op = yt.run_map(write_statistics, table, table, format=None, sync=False)
        op.wait()
        assert sorted(list(op.get_job_statistics()["custom"])) == sorted(["row_count", "python_job_preparation_time"])
        assert op.get_job_statistics()["custom"]["row_count"] == {"$": {"completed": {"map": {"count": 2, "max": 1, "sum": 2, "min": 1}}}}
        check(yt.read_table(table), [{"x": 1}, {"y": 2}], ordered=False)

    @add_failed_operation_stderrs_to_error_message
    def test_python_operations_and_file_cache(self):
        def func(row):
            yield row

        input = TEST_DIR + "/input"
        output = TEST_DIR + "/output"
        yt.write_table(input, [{"x": 1}, {"y": 2}])

        # Some strange things are happen.
        # Sometimes in the first iteration some modules occurred to be unimported (like yt_env.pyc).
        # So we only tests that regularly operation files are the same in sequential runs.
        failures = 0
        for i in xrange(3):
            yt.run_map(func, input, output)
            files_in_cache = list(yt.search("//tmp/yt_wrapper/file_storage", node_type="link"))
            assert len(files_in_cache) > 0

            yt.run_map(func, input, output)
            files_in_cache_again = list(yt.search("//tmp/yt_wrapper/file_storage", node_type="link"))
            if sorted(files_in_cache) != sorted(files_in_cache_again):
                failures += 1

        assert failures <= 1

    @add_failed_operation_stderrs_to_error_message
    def test_python_operations_with_local_python(self):
        def func(row):
            yield row

        input = TEST_DIR + "/input"
        output = TEST_DIR + "/output"
        yt.write_table(input, [{"x": 1}, {"y": 2}])

        with set_config_option("pickling/use_local_python_in_jobs", True):
            yt.run_map(func, input, output)

        check(yt.read_table(output), [{"x": 1}, {"y": 2}], ordered=False)

    @add_failed_operation_stderrs_to_error_message
    def test_python_operations_in_local_mode(self):
        with set_config_option("is_local_mode", True):
            with set_config_option("pickling/enable_local_files_usage_in_job", True):
                old_tmp_dir = yt.config["local_temp_directory"]
                yt.config["local_temp_directory"] = tempfile.mkdtemp(dir=old_tmp_dir)

                os.chmod(yt.config["local_temp_directory"], 0o755)

                try:
                    def foo(rec):
                        yield rec

                    table = TEST_DIR + "/table"

                    yt.write_table(table, [{"x": 1}, {"y": 2}])
                    yt.run_map(foo, table, table, format=None)
                    check(yt.read_table(table), [{"x": 1}, {"y": 2}], ordered=False)
                finally:
                    yt.config["local_temp_directory"] = old_tmp_dir

    @add_failed_operation_stderrs_to_error_message
    def test_cross_format_operations(self):
        @yt.raw
        def reformat(rec):
            values = rec.strip().split(b"\t", 2)
            yield b"\t".join(b"=".join([k, v]) for k, v in izip([b"k", b"s", b"v"], values)) + b"\n"

        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"

        yt.config["tabular_data_format"] = yt.format.YamrFormat(has_subkey=True)

        # Enable progress printing in this test
        old_level = logger.LOGGER.level
        logger.LOGGER.setLevel(logging.INFO)
        try:
            yt.write_table(table, [b"0\ta\tA\n", b"1\tb\tB\n"], raw=True)
            yt.run_map(reformat, table, other_table, output_format=yt.format.DsvFormat())
            assert sorted(yt.read_table(other_table, format="dsv", raw=True)) == \
                   [b"k=0\ts=a\tv=A\n", b"k=1\ts=b\tv=B\n"]
        finally:
            yt.config["tabular_data_format"] = None
            logger.LOGGER.setLevel(old_level)

        yt.write_table(table, [b"1\t2\t3\n"], format="<has_subkey=true>yamr", raw=True)
        yt.run_map(reformat, table, table, input_format="<has_subkey=true>yamr", output_format="dsv")
        yt.run_map("cat", table, table, input_format="dsv", output_format="dsv")
        assert list(yt.read_table(table, format=yt.format.DsvFormat(), raw=True)) == [b"k=1\ts=2\tv=3\n"]

    def test_python_operations_io(self):
        """All access (except read-only) to stdin/out during the operation should be disabled."""
        table = TEST_DIR + "/table_io_test"

        yt.write_table(table, [{"x": 1}, {"y": 2}])

        def print_(rec):
            print('message')

        @yt.raw
        def write(rec):
            sys.stdout.write('message')

        @yt.raw
        def input_(rec):
            input()

        @yt.raw
        def read(rec):
            sys.stdin.read()

        @yt.raw
        def close(rec):
            sys.stdin.close()

        test_mappers = [print_, write, input_, read, close]
        for mapper in test_mappers:
            with pytest.raises(yt.YtError):
                yt.run_map(mapper, table, table)

    @add_failed_operation_stderrs_to_error_message
    def test_many_output_tables(self):
        table = TEST_DIR + "/table"
        output_tables = []
        for i in xrange(10):
            output_tables.append(TEST_DIR + "/temp%d" % i)
        append_table = TEST_DIR + "/temp_special"
        yt.write_table(table, [{"x": "1", "y": "1"}])
        yt.write_table(append_table, [{"x": "1", "y": "1"}])

        yt.run_map("PYTHONPATH=. ./many_output.py yt",
                   table,
                   output_tables + [TablePath(append_table, append=True)],
                   files=get_test_file_path("many_output.py"),
                   format=yt.DsvFormat())

        for table in output_tables:
            assert yt.row_count(table) == 1
        check([{"x": "1", "y": "1"}, {"x": "10", "y": "10"}], yt.read_table(append_table), ordered=False)

    def test_attached_mode_simple(self):
        table = TEST_DIR + "/table"

        yt.config["detached"] = 0
        try:
            yt.write_table(table, [{"x": 1}])
            yt.run_map("cat", table, table)
            check(yt.read_table(table), [{"x": 1}])
            yt.run_merge(table, table)
            check(yt.read_table(table), [{"x": 1}])
        finally:
            yt.config["detached"] = 1

    def test_attached_mode_op_aborted(self, yt_env):
        script = """
from __future__ import print_function

import yt.wrapper as yt
import sys

input, output, pythonpath = sys.argv[1:4]
yt.config["proxy"]["request_retry_timeout"] = 2000
yt.config["proxy"]["request_retry_count"] = 1
yt.config["detached"] = False
op = yt.run_map("sleep 100", input, output, format="json", spec={"mapper": {"environment": {"PYTHONPATH": pythonpath}}}, sync=False)
print(op.id)

"""
        dir_ = yt_env.env.path
        with tempfile.NamedTemporaryFile(mode="w", dir=dir_, prefix="mapper", delete=False) as file:
            file.write(script)

        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1}])

        env = {
            "PYTHONPATH": PYTHONPATH,
            "YT_CONFIG_PATCHES": dumps_yt_config()
        }

        op_id = subprocess.check_output([sys.executable, file.name, table, table, PYTHONPATH],
                                        env=env, stderr=sys.stderr).strip()
        time.sleep(3)

        assert yt.get("//sys/operations/{0}/@state".format(op_id)) == "aborted"

    def test_abort_operation(self):
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1}])
        op = yt.run_map("sleep 15; cat", table, table, sync=False)
        op.abort()
        assert op.get_state() == "aborted"

    def test_complete_operation(self):
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1}])
        op = yt.run_map("sleep 15; cat", table, table, sync=False)
        while not op.get_state().is_running():
            time.sleep(0.2)
        op.complete()
        assert op.get_state() == "completed"

    def test_suspend_resume(self):
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"key": 1}])
        try:
            op = yt.run_map_reduce(
                "sleep 0.5; cat",
                "sleep 0.5; cat",
                table,
                table,
                sync=False,
                reduce_by=["key"],
                spec={"map_locality_timeout": 0, "reduce_locality_timeout": 0})

            time.sleep(0.5)
            op.suspend()
            assert op.get_state() == "running"
            time.sleep(2.5)
            assert op.get_state() == "running"
            op.resume()
            op.wait(timeout=10)
            assert op.get_state() == "completed"
        finally:
            if op.get_state() not in ["completed", "failed", "aborted"]:
                op.abort()

    def test_reduce_combiner(self):
        table = TEST_DIR + "/table"
        output_table = TEST_DIR + "/output_table"
        yt.write_table(table, [{"x": 1}, {"y": 2}])

        yt.run_map_reduce(mapper=None, reduce_combiner="cat", reducer="cat", reduce_by=["x"],
                          source_table=table, destination_table=output_table)
        check([{"x": 1}, {"y": 2}], list(yt.read_table(table)))

    def test_reduce_differently_sorted_table(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.create("table", table)
        yt.run_sort(table, sort_by=["a", "b"])

        with pytest.raises(yt.YtError):
            # No reduce_by
            yt.run_reduce("cat", source_table=table, destination_table=other_table, sort_by=["a"])

        with pytest.raises(yt.YtError):
            yt.run_reduce("cat", source_table=table, destination_table=other_table, reduce_by=["c"])

    @add_failed_operation_stderrs_to_error_message
    def test_yamred_dsv(self):
        def foo(rec):
            yield rec

        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": "1", "y": "2"}])

        yt.run_map(foo, table, table,
                   input_format=yt.create_format("<key_column_names=[\"y\"]>yamred_dsv"),
                   output_format=yt.YamrFormat(has_subkey=False, lenval=False))
        check([{"key": "2", "value": "x=1"}], list(yt.read_table(table)))

    def test_schemaful_dsv(self):
        def foo(rec):
            yield rec

        table = TEST_DIR + "/table"
        yt.write_table(table, [b"x=1\ty=2\n", b"x=\\n\tz=3\n"], raw=True, format=yt.DsvFormat())
        check([b"1\n", b"\\n\n"],
              sorted(list(yt.read_table(table, format=yt.SchemafulDsvFormat(columns=["x"]), raw=True))))

        yt.run_map(foo, table, table, format=yt.SchemafulDsvFormat(columns=["x"]))
        check([b"x=1\n", b"x=\\n\n"], sorted(list(yt.read_table(table, format=yt.DsvFormat(), raw=True))))

    @add_failed_operation_stderrs_to_error_message
    def test_reduce_aggregator(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"
        yt.write_table(table, [{"x": 1, "y": 2}, {"x": 0, "y": 3}, {"x": 1, "y": 4}])

        @yt.reduce_aggregator
        def reducer(row_groups):
            sum_y = 0
            for k, rows in row_groups:
                for row in rows:
                    sum_y += int(row["y"])
            yield {"sum_y": sum_y}

        yt.run_sort(table, sort_by=["x"])
        yt.run_reduce(reducer, table, other_table, reduce_by=["x"])
        assert [{"sum_y": 9}] == list(yt.read_table(other_table))

    def test_operation_receives_spec_from_config(self):
        memory_limit = yt.config["memory_limit"]
        yt.config["memory_limit"] = 123
        check_input_fully_consumed = yt.config["yamr_mode"]["check_input_fully_consumed"]
        yt.config["yamr_mode"]["check_input_fully_consumed"] = not check_input_fully_consumed
        use_yamr_descriptors = yt.config["yamr_mode"]["use_yamr_style_destination_fds"]
        yt.config["yamr_mode"]["use_yamr_style_destination_fds"] = not use_yamr_descriptors
        yt.config["table_writer"] = {"max_row_weight": 8 * 1024 * 1024}

        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1}])
        try:
            op = yt.run_map("sleep 1; cat", table, table, sync=False)
            spec = yt.get_attribute("//sys/operations/{0}".format(op.id), "spec")
            assert spec["mapper"]["memory_limit"] == 123
            assert parse_bool(spec["mapper"]["check_input_fully_consumed"]) != check_input_fully_consumed
            assert parse_bool(spec["mapper"]["use_yamr_descriptors"]) != use_yamr_descriptors
            assert spec["job_io"]["table_writer"]["max_row_weight"] == 8 * 1024 * 1024
        finally:
            yt.config["memory_limit"] = memory_limit
            yt.config["yamr_mode"]["check_input_fully_consumed"] = check_input_fully_consumed
            yt.config["yamr_mode"]["use_yamr_style_destination_fds"] = use_yamr_descriptors
            yt.config["table_writer"] = {}
            try:
                op.abort()
            except yt.YtError:
                pass

    @add_failed_operation_stderrs_to_error_message
    def test_operation_start_finish_methods(self):
        table = TEST_DIR + "/table"
        other_table = TEST_DIR + "/other_table"

        yt.write_table(table, [{"x": 1}, {"x": 2}])
        yt.run_map(AggregateMapper(), table, other_table)
        assert [{"sum": 3}] == list(yt.read_table(other_table))
        yt.write_table(table, [{"x": 1, "y": 2}, {"x": 0, "y": 3}, {"x": 1, "y": 4}])
        yt.run_sort(table, sort_by=["x"])
        yt.run_reduce(AggregateReducer(), table, other_table, reduce_by=["x"])
        check([{"sum": 1}, {"sum": 2}, {"sum": 9}], list(yt.read_table(other_table)))

    @add_failed_operation_stderrs_to_error_message
    def test_create_modules_archive(self):
        def foo(rec):
            yield rec

        table = TEST_DIR + "/table"

        try:
            yt.config["pickling"]["create_modules_archive_function"] = \
                lambda tempfiles_manager: create_modules_archive_default(tempfiles_manager, False, None)
            yt.run_map(foo, table, table)

            with TempfilesManager(remove_temp_files=True, directory=yt.config["local_temp_directory"]) as tempfiles_manager:
                yt.config["pickling"]["create_modules_archive_function"] = lambda: create_modules_archive_default(tempfiles_manager, False, None)
                yt.run_map(foo, table, table)

            with TempfilesManager(remove_temp_files=True, directory=yt.config["local_temp_directory"]) as tempfiles_manager:
                yt.config["pickling"]["create_modules_archive_function"] = lambda: create_modules_archive_default(tempfiles_manager, False, None)[0]["filename"]
                yt.run_map(foo, table, table)

            yt.config["pickling"]["create_modules_archive_function"] = CreateModulesArchive()
            yt.run_map(foo, table, table)

        finally:
            yt.config["pickling"]["create_modules_archive_function"] = None

    def test_pickling(self):
        def foo(rec):
            import test_module
            assert test_module.TEST == 1
            yield rec

        with tempfile.NamedTemporaryFile(mode="w",
                                         suffix=".py",
                                         dir=TESTS_SANDBOX,
                                         prefix="test_pickling",
                                         delete=False) as f:
            f.write("TEST = 1")

        with set_config_option("pickling/additional_files_to_archive", [(f.name, "test_module.py")]):
            table = TEST_DIR + "/table"
            yt.write_table(table, [{"x": 1}])
            yt.run_map(foo, table, table)

    @add_failed_operation_stderrs_to_error_message
    def test_is_inside_job(self):
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1}])

        def mapper(rec):
            yield {"flag": str(yt.is_inside_job()).lower()}

        yt.run_map(mapper, table, table)
        assert not yt.is_inside_job()
        assert list(yt.read_table(table)) == [{"flag": "true"}]

    def test_retrying_operation_count_limit_exceeded(self):
        # TODO(ignat): Rewrite test without sleeps.
        old_value = yt.config["start_operation_retries"]["retry_timeout"]
        yt.config["start_operation_retries"]["retry_timeout"] = 2000

        yt.create("map_node", "//sys/pools/with_operation_count_limit", attributes={"max_operation_count": 1})
        time.sleep(1)

        try:
            table = TEST_DIR + "/table"
            yt.write_table(table, [{"x": 1}, {"x": 2}])

            def run_operation(index):
                return yt.run_map(
                    "cat; sleep 5",
                    table,
                    TEST_DIR + "/output_" + str(index),
                    sync=False,
                    spec={"pool": "with_operation_count_limit"})

            ops = []
            start_time = time.time()
            ops.append(run_operation(1))
            assert time.time() - start_time < 5.0
            ops.append(run_operation(2))
            assert time.time() - start_time > 5.0

            for op in ops:
                op.wait()

            assert time.time() - start_time > 10.0

        finally:
            yt.config["start_operation_retries"]["retry_timeout"] = old_value

    @add_failed_operation_stderrs_to_error_message
    def test_reduce_key_modification(self):
        def reducer(key, recs):
            rec = next(recs)
            key["x"] = int(rec["y"]) + 10
            yield key

        def reducer_that_yileds_key(key, recs):
            for rec in recs:
                pass
            yield key

        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1, "y": 1}, {"x": 1, "y": 2}, {"x": 2, "y": 3}])
        yt.run_sort(table, table, sort_by=["x"])

        with pytest.raises(yt.YtOperationFailedError):
            yt.run_reduce(reducer, table, TEST_DIR + "/other", reduce_by=["x"], format="json")

        yt.run_reduce(reducer_that_yileds_key, table, TEST_DIR + "/other", reduce_by=["x"], format="json")
        check([{"x": 1}, {"x": 2}], yt.read_table(TEST_DIR + "/other"), ordered=False)

    def test_disable_yt_accesses_from_job(self, yt_env):
        if yt.config["backend"] == "native":
            pytest.skip()

        first_script = """\
from __future__ import print_function

import yt.wrapper as yt

def mapper(rec):
    yield rec

yt.config["proxy"]["url"] = "{0}"
yt.config["pickling"]["enable_tmpfs_archive"] = False
print(yt.run_map(mapper, "{1}", "{2}", sync=False).id)
"""
        second_script = """\
from __future__ import print_function

import yt.wrapper as yt

def mapper(rec):
    yt.get("//@")
    yield rec

if __name__ == "__main__":
    yt.config["proxy"]["url"] = "{0}"
    yt.config["pickling"]["enable_tmpfs_archive"] = False
    print(yt.run_map(mapper, "{1}", "{2}", sync=False).id)
"""
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1}, {"x": 2}])

        dir_ = yt_env.env.path
        for script in [first_script, second_script]:
            with tempfile.NamedTemporaryFile("w", dir=dir_, prefix="mapper", delete=False) as f:
                mapper = script.format(yt.config["proxy"]["url"],
                                       table,
                                       TEST_DIR + "/other_table")
                f.write(mapper)

            op_id = subprocess.check_output([sys.executable, f.name],
                                            env={"PYTHONPATH": PYTHONPATH}).strip()
            op_path = "//sys/operations/{0}".format(op_id)
            while not yt.exists(op_path) \
                    or yt.get(op_path + "/@state") not in ["aborted", "failed", "completed"]:
                time.sleep(0.2)
            assert yt.get(op_path + "/@state") == "failed"

            job_id = yt.list(op_path + "/jobs", attributes=["error"])[0]
            stderr_path = os.path.join(op_path, "jobs", job_id, "stderr")

            while not yt.exists(stderr_path):
                time.sleep(0.2)

            assert b"Did you forget to surround" in yt.read_file(stderr_path).read()

    @add_failed_operation_stderrs_to_error_message
    def test_table_and_row_index_from_job(self):
        @yt.aggregator
        def mapper(rows):
            for row in rows:
                assert "@table_index" in row
                assert "@row_index" in row
                row["table_index"] = int(row["@table_index"])
                del row["@table_index"]
                row["row_index"] = int(row["@row_index"])
                del row["@row_index"]
                yield row

        tableA = TEST_DIR + "/tableA"
        yt.write_table(tableA, [{"x": 1}, {"y": 1}])

        tableB = TEST_DIR + "/tableB"
        yt.write_table(tableB, [{"x": 2}])

        outputTable = TEST_DIR + "/output"

        yt.run_map(mapper, [tableA, tableB], outputTable, format=yt.YsonFormat(control_attributes_mode="row_fields"),
                   spec={"job_io": {"control_attributes": {"enable_row_index": True}}, "ordered": True})

        result = sorted(list(yt.read_table(outputTable, raw=False, format=yt.YsonFormat())),
                        key=lambda item: (item["table_index"], item["row_index"]))

        assert [
            {"table_index": 0, "row_index": 0, "x": 1},
            {"table_index": 0, "row_index": 1, "y": 1},
            {"table_index": 1, "row_index": 0, "x": 2}
        ] == result

    def test_reduce_sort_by(self):
        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1, "y": 1}])
        yt.run_sort(table, sort_by=["x", "y"])
        op = yt.run_reduce("cat", table, table, format=yt.JsonFormat(), reduce_by=["x"], sort_by=["x", "y"])
        assert "sort_by" in op.get_attributes()["spec"]

    @add_failed_operation_stderrs_to_error_message
    def test_eggs_file_usage_from_operation(self, yt_env):
        script = """\
from __future__ import print_function

import yt.wrapper as yt
from module_in_egg import hello_provider

def mapper(rec):
    yield {{"x": hello_provider.get_message()}}

if __name__ == "__main__":
    yt.config["pickling"]["enable_tmpfs_archive"] = False
    print(yt.run_map(mapper, "{1}", "{2}", sync=False).id)
"""
        yt.write_table(TEST_DIR + "/table", [{"x": 1, "y": 1}])

        dir_ = yt_env.env.path
        with tempfile.NamedTemporaryFile("w", dir=dir_, prefix="mapper", delete=False) as f:
            mapper = script.format(yt.config["proxy"]["url"],
                                   TEST_DIR + "/table",
                                   TEST_DIR + "/other_table")
            f.write(mapper)

        module_egg = build_python_egg(get_test_file_path("yt_test_module"), temp_dir=dir_)

        env = {
            "YT_CONFIG_PATCHES": dumps_yt_config(),
            "PYTHONPATH": os.pathsep.join([module_egg, PYTHONPATH])
        }

        operation_id = subprocess.check_output([sys.executable, f.name], env=env).strip()

        op = yt.Operation("map", operation_id)
        op.wait()
        assert list(yt.read_table(TEST_DIR + "/other_table")) == [{"x": "hello"}]

    def test_operations_tracker(self):
        tracker = yt.OperationsTracker()

        # To enable progress printing
        old_level = logger.LOGGER.level
        logger.LOGGER.setLevel(logging.INFO)
        try:
            with pytest.raises(yt.YtError):
                tracker.add_by_id("123")

            table = TEST_DIR + "/table"
            yt.write_table(table, [{"x": 1, "y": 1}])

            op1 = yt.run_map("sleep 30; cat", table, TEST_DIR + "/out1", sync=False)
            op2 = yt.run_map("sleep 30; cat", table, TEST_DIR + "/out2", sync=False)

            tracker.add(op1)
            tracker.add(op2)
            tracker.abort_all()

            assert op1.get_state() == "aborted"
            assert op2.get_state() == "aborted"

            op1 = yt.run_map("sleep 2; cat", table, TEST_DIR + "/out1", sync=False)
            op2 = yt.run_map("sleep 2; cat", table, TEST_DIR + "/out2", sync=False)
            tracker.add_by_id(op1.id)
            tracker.add_by_id(op2.id)
            tracker.wait_all()

            assert op1.get_state().is_finished()
            assert op2.get_state().is_finished()

            tracker.add(yt.run_map("false", table, TEST_DIR + "/out", sync=False))
            with pytest.raises(yt.YtError):
                tracker.wait_all(check_result=True)

            assert not tracker.operations

            op = yt.run_map("cat", table, TEST_DIR + "/out", sync=False)
            tracker.add(op)
            tracker.wait_all(keep_finished=True)
            assert op.id in tracker.operations

            tracker.wait_all(keep_finished=True)
            tracker.abort_all()

            with tracker:
                op = yt.run_map("sleep 2; true", table, TEST_DIR + "/out", sync=False)
                tracker.add(op)
            assert op.get_state() == "completed"

            with pytest.raises(RuntimeError):
                with tracker:
                    op = yt.run_map("sleep 100; cat", table, TEST_DIR + "/out", sync=False)
                    tracker.add(op)
                    raise RuntimeError("error")

            assert op.get_state() == "aborted"
        finally:
            logger.LOGGER.setLevel(old_level)

    @add_failed_operation_stderrs_to_error_message
    def test_enable_dynamic_libraries_collection(self):
        def mapper(rec):
            assert "_shared" in os.environ["LD_LIBRARY_PATH"]
            for root, dirs, files in os.walk("."):
                if "libgetnumber.so" in files:
                    break
            else:
                assert False, "Dependency libgetnumber.so not collected"
            yield rec

        table = TEST_DIR + "/table"
        yt.write_table(table, [{"x": 1, "y": 1}])

        if not which("g++"):
            raise RuntimeError("g++ not found")

        libs_dir = os.path.join(TESTS_SANDBOX, "test_enable_dynamic_libraries_collection_libs")
        makedirp(libs_dir)

        get_number_lib = get_test_file_path("getnumber.cpp")
        subprocess.check_call(["g++", get_number_lib, "-shared", "-fPIC", "-o", os.path.join(libs_dir, "libgetnumber.so")])

        dependant_lib = get_test_file_path("yt_test_lib.cpp")
        dependant_lib_output = os.path.join(libs_dir, "yt_test_dynamic_libraries_collection.so")
        subprocess.check_call(["g++", dependant_lib, "-shared", "-o", dependant_lib_output,
                               "-L", libs_dir, "-l", "getnumber", "-fPIC"])

        # Adding this pseudo-module to sys.modules and ensuring it will be collected with
        # its dependency (libgetnumber.so)
        module = imp.new_module("yt_test_dynamic_libraries_collection")
        module.__file__ = dependant_lib_output
        sys.modules["yt_test_dynamic_libraries_collection"] = module
        old_ld_library_path = os.environ.get("LD_LIBRARY_PATH", "")
        os.environ["LD_LIBRARY_PATH"] = os.pathsep.join([old_ld_library_path, libs_dir])
        try:
            with set_config_option("pickling/dynamic_libraries/enable_auto_collection", True):
                 with set_config_option("pickling/dynamic_libraries/library_filter",
                                        lambda lib: not lib.startswith("/lib")):
                    yt.run_map(mapper, table, TEST_DIR + "/out")
        finally:
            del sys.modules["yt_test_dynamic_libraries_collection"]
            if old_ld_library_path:
                os.environ["LD_LIBRARY_PATH"] = old_ld_library_path

    @add_failed_operation_stderrs_to_error_message
    def test_mount_tmpfs_in_sandbox(self, yt_env):
        if not ENABLE_JOB_CONTROL:
            pytest.skip()

        def foo(rec):
            yield rec

        def get_spec_option(id, name):
            return yt.get("//sys/operations/{0}/@spec/".format(id) + name)

        with set_config_option("mount_sandbox_in_tmpfs/enable", True):
            table = TEST_DIR + "/table"
            file = TEST_DIR + "/test_file"
            table_file = TEST_DIR + "/test_table_file"

            dir_ = yt_env.env.path
            with tempfile.NamedTemporaryFile(dir=dir_, prefix="local_file", delete=False) as local_file:
                local_file.write(b"bbbbb")
            yt.write_table(table, [{"x": 1}, {"y": 2}])
            yt.write_table(table_file, [{"x": 1}, {"y": 2}])
            yt.write_file(file, b"aaaaa")
            table_file_object = yt.FilePath(table_file, attributes={"format": "json", "disk_size": 1000})
            op = yt.run_map(foo, table, table, local_files=[local_file.name], yt_files=[file, table_file_object], format=None)
            check(yt.read_table(table), [{"x": 1}, {"y": 2}], ordered=False)

            tmpfs_size = get_spec_option(op.id, "mapper/tmpfs_size")
            memory_limit = get_spec_option(op.id, "mapper/memory_limit")
            assert tmpfs_size > 8 * 1024
            assert memory_limit - tmpfs_size == 512 * 1024 * 1024
            assert get_spec_option(op.id, "mapper/tmpfs_path") == "."

    @add_failed_operation_stderrs_to_error_message
    def test_functions_with_context(self):
        @yt.with_context
        def mapper(row, context):
            yield {"row_index": context.row_index}

        @yt.with_context
        def reducer(key, rows, context):
            for row in rows:
                yield {"row_index": context.row_index}

        input = TEST_DIR + "/input"
        output = TEST_DIR + "/output"
        yt.write_table(input, [{"x": 1, "y": "a"}, {"x": 1, "y": "b"}, {"x": 2, "y": "b"}])
        yt.run_map(mapper, input, output,
                   format=yt.YsonFormat(),
                   spec={"job_io": {"control_attributes": {"enable_row_index": True}}})

        check(yt.read_table(output), [{"row_index": index} for index in xrange(3)], ordered=True)

        yt.run_sort(input, input, sort_by=["x"])
        yt.run_reduce(reducer, input, output,
                      reduce_by=["x"],
                      format=yt.YsonFormat(),
                      spec={"job_io": {"control_attributes": {"enable_row_index": True}}})
        check(yt.read_table(output), [{"row_index": index} for index in xrange(3)], ordered=True)
