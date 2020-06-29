import itertools

import pytest

from yt.environment.helpers import assert_items_equal, wait
from yt_env_setup import YTEnvSetup, unix_only, parametrize_external
from yt_commands import *
from yt.yson import YsonEntity


##################################################################

class TestSchedulerReduceCommands(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 5
    NUM_SCHEDULERS = 1
    USE_DYNAMIC_TABLES = True

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "operations_update_period": 10,
            "running_jobs_update_period": 10,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10,
            "reduce_operation_options": {
                "job_splitter": {
                    "min_job_time": 5000,
                    "min_total_data_size": 1024,
                    "update_period": 100,
                    "candidate_percentile": 0.8,
                    "max_jobs_per_split": 3,
                }
            }
        }
    }

    def _create_simple_dynamic_table(self, path, **attributes):
        if "schema" not in attributes:
            attributes.update({"schema": [
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "string"}]
            })
        create_dynamic_table(path, **attributes)

    # TODO(max42): eventually remove this test as it duplicates unittest TSortedChunkPoolTest/SortedReduceSimple.
    @authors("psushin", "klyachin")
    @unix_only
    def test_tricky_chunk_boundaries(self):
        create("table", "//tmp/in1")
        write_table(
            "//tmp/in1",
            [
                {"key": "0", "value": 1},
                {"key": "2", "value": 2}
            ],
            sorted_by = ["key", "value"])

        create("table", "//tmp/in2")
        write_table(
            "//tmp/in2",
            [
                {"key": "2", "value": 6},
                {"key": "5", "value": 8}
            ],
            sorted_by = ["key", "value"])

        create("table", "//tmp/out")

        reduce(
            in_=["//tmp/in1{key}", "//tmp/in2{key}"],
            out=["<sorted_by=[key]>//tmp/out"],
            command="uniq",
            reduce_by="key",
            spec={"reducer": {"format": yson.loads("<line_prefix=tskv>dsv")},
                  "data_size_per_job": 1})

        assert read_table("//tmp/out") == \
            [
                {"key": "0"},
                {"key": "2"},
                {"key": "5"}
            ]

        assert get("//tmp/out/@sorted")

    @authors("klyachin")
    @unix_only
    def test_cat(self):
        create("table", "//tmp/in1")
        write_table(
            "//tmp/in1",
            [
                {"key": 0, "value": 1},
                {"key": 2, "value": 2},
                {"key": 4, "value": 3},
                {"key": 7, "value": 4}
            ],
            sorted_by = "key")

        create("table", "//tmp/in2")
        write_table(
            "//tmp/in2",
            [
                {"key": -1,"value": 5},
                {"key": 1, "value": 6},
                {"key": 3, "value": 7},
                {"key": 5, "value": 8}
            ],
            sorted_by = "key")

        create("table", "//tmp/out")

        reduce(
            in_=["//tmp/in1", "//tmp/in2"],
            out="<sorted_by=[key]>//tmp/out",
            reduce_by="key",
            command="cat",
            spec={"reducer": {"format": "dsv"}})

        assert read_table("//tmp/out") == \
            [
                {"key": "-1","value": "5"},
                {"key": "0", "value": "1"},
                {"key": "1", "value": "6"},
                {"key": "2", "value": "2"},
                {"key": "3", "value": "7"},
                {"key": "4", "value": "3"},
                {"key": "5", "value": "8"},
                {"key": "7", "value": "4"}
            ]

        assert get("//tmp/out/@sorted")

    @authors("psushin")
    @unix_only
    def test_column_filter(self):
        create("table", "//tmp/in1")
        set("//tmp/in1/@optimize_for", "scan")

        write_table(
            "//tmp/in1",
            [
                {"key": 0, "value": 0},
                {"key": 0, "value": 0},
                {"key": 0, "value": 1},
                {"key": 7, "value": 4}
            ],
            sorted_by = ["key", "value"])


        create("table", "//tmp/out")

        with pytest.raises(YtError):
        # All reduce by columns must be included in column filter.
            reduce(
                in_="//tmp/in1{key}",
                out="//tmp/out",
                reduce_by=["key", "value"],
                command="cat",
                spec={"reducer": {"format": "dsv"}})

        reduce(
            in_="//tmp/in1{key}[(0, 1):]",
            out="//tmp/out",
            reduce_by=["key"],
            command="cat")

        assert read_table("//tmp/out") == \
            [
                {"key": 0},
                {"key": 7}
            ]

    @authors("dakovalkov")
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_rename_columns_simple(self, optimize_for):
        create("table", "//tmp/t1", attributes={
                "schema": [{"name": "a", "type": "int64", "sort_order": "ascending"},
                           {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
            })
        create("table", "//tmp/t2", attributes={
                "schema": [{"name": "a2", "type": "int64", "sort_order": "ascending"},
                           {"name": "b2", "type": "int64"}],
                "optimize_for": optimize_for
            })
        create("table", "//tmp/t3", attributes={
                "schema": [{"name": "a3", "type": "int64", "sort_order": "ascending"},
                           {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
            })

        create("table", "//tmp/tout")

        write_table("//tmp/t1", [{"a": 42, "b": 1}])
        write_table("//tmp/t2", [{"a2": 42, "b2": 2}])
        write_table("//tmp/t3", [{"a3": 42, "b": 3}])

        op = reduce(
            in_=["//tmp/t1", "<rename_columns={a2=a;b2=b}>//tmp/t2", "<rename_columns={a3=a}>//tmp/t3"],
            out="//tmp/tout",
            reduce_by=["a"],
            command="cat",
            spec={"job_count": 3, # We are trying to divide rows in different jobs.
                  "reducer": {"format": "dsv"}})

        assert read_table("//tmp/tout") == [{"a": "42", "b": "1"},
                                            {"a": "42", "b": "2"},
                                            {"a": "42", "b": "3"}]

        completed = get(op.get_path() + "/@progress/jobs/completed")
        assert completed["total"] == 1 # Actualy all rows should be in one job despite job_count > 1

    @authors("dakovalkov")
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_rename_columns_foreign_table(self, optimize_for):
        create("table", "//tmp/t1", attributes={
                "schema": [{"name": "a", "type": "int64", "sort_order": "ascending"},
                           {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
            })
        create("table", "//tmp/t2", attributes={
                "schema": [{"name": "a2", "type": "int64", "sort_order": "ascending"},
                           {"name": "b2", "type": "int64"}],
                "optimize_for": optimize_for
            })

        create("table", "//tmp/tout")

        write_table("//tmp/t1", [{"a": 42, "b": 1}])
        write_table("//tmp/t2", [{"a2": 42, "b2": 2},
                                 {"a2": 43, "b2": 3}])

        reduce(
            in_=["//tmp/t1", "<foreign=%true;rename_columns={a2=a;b2=b}>//tmp/t2"],
            out="//tmp/tout",
            reduce_by=["a"],
            join_by=["a"],
            command="cat",
            spec={"reducer": {"enable_input_table_index": False}})

        assert read_table("//tmp/tout") == [{"a": 42, "b": 1},
                                            {"a": 42, "b": 2}]

    @authors("dakovalkov")
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_rename_columns_teleport_table(self, optimize_for):
        create("table", "//tmp/t1", attributes={
                "schema": [{"name": "a", "type": "int64", "sort_order": "ascending"},
                           {"name": "b", "type": "int64"}],
                "optimize_for": optimize_for
            })
        create("table", "//tmp/t2", attributes={
                "schema": [{"name": "a2", "type": "int64", "sort_order": "ascending"},
                           {"name": "b2", "type": "int64"}],
                "optimize_for": optimize_for
            })

        create("table", "//tmp/tout")

        write_table("//tmp/t1", [{"a": 42, "b": 1}])
        write_table("//tmp/t2", [{"a2": 43, "b2": 3}])

        with pytest.raises(YtError):
            reduce(
                in_=["//tmp/t1", "<teleport=%true;rename_columns={a2=a;b2=b}>//tmp/t2"],
                out="<teleport=%true>//tmp/tout",
                reduce_by=["a"],
                command="cat",
                spec={"reducer": {"format": "dsv"}})

    @authors("psushin", "klyachin")
    @unix_only
    def test_control_attributes_yson(self):
        create("table", "//tmp/in1")
        write_table(
            "//tmp/in1",
            {"key": 4, "value": 3},
            sorted_by = "key")

        create("table", "//tmp/in2")
        write_table(
            "//tmp/in2",
            {"key": 1, "value": 6},
            sorted_by = "key")

        create("table", "//tmp/out")

        op = reduce(
            in_=["//tmp/in1", "//tmp/in2"],
            out="<sorted_by=[key]>//tmp/out",
            reduce_by="key",
            command="cat > /dev/stderr",
            spec={
                "reducer" : {"format" : yson.loads("<format=text>yson")},
                "job_io" : {
                    "control_attributes" : {
                        "enable_table_index" : "true",
                        "enable_row_index" : "true"}
                    },
                "job_count" : 1})


        expected_stderr = \
"""<"table_index"=1;>#;
<"row_index"=0;>#;
{"key"=1;"value"=6;};
<"table_index"=0;>#;
<"row_index"=0;>#;
{"key"=4;"value"=3;};
"""
        check_all_stderrs(op, expected_stderr, 1)

        # Test only one row index with only one input table.
        op = reduce(
            in_=["//tmp/in1"],
            out="<sorted_by=[key]>//tmp/out",
            command="cat > /dev/stderr",
            reduce_by="key",
            spec={
                "reducer" : {"format" : yson.loads("<format=text>yson")},
                "job_io" : {
                    "control_attributes" : { "enable_row_index" : "true" }
                },
                "job_count" : 1})

        expected_stderr = \
"""<"row_index"=0;>#;
{"key"=4;"value"=3;};
"""
        check_all_stderrs(op, expected_stderr, 1)

    @authors("savrus", "klyachin")
    @unix_only
    def test_cat_teleport(self):
        schema = make_schema([
            {"name": "key", "type": "int64", "sort_order": "ascending"},
            {"name": "value", "type": "int64", "sort_order": "ascending"}],
            unique_keys=True)
        create("table", "//tmp/in1", attributes={"schema": schema})
        create("table", "//tmp/in2", attributes={"schema": schema})
        create("table", "//tmp/in3", attributes={"schema": schema})
        create("table", "//tmp/in4", attributes={"schema": schema})

        write_table(
            "//tmp/in1",
            [
                {"key": 0, "value": 1},
                {"key": 2, "value": 2},
                {"key": 4, "value": 3},
                {"key": 7, "value": 4}
            ])
        write_table(
            "//tmp/in2",
            [
                {"key": 8, "value": 5},
                {"key": 9, "value": 6},
            ])
        write_table(
            "//tmp/in3",
            [ {"key": 8, "value": 1}, ])
        write_table(
            "//tmp/in4",
            [ {"key": 9, "value": 7}, ])

        assert get("//tmp/in1/@sorted_by") == ["key", "value"]
        assert get("//tmp/in1/@schema/@unique_keys")

        create("table", "//tmp/out1")
        create("table", "//tmp/out2")
        create("table", "//tmp/out3", attributes={"schema":
            make_schema([
                {"name": "key", "type": "int64", "sort_order": "ascending"},
                {"name": "value", "type": "int64"}],
                unique_keys=True)
            })

        with pytest.raises(YtError):
            reduce(
                in_ = ['<teleport=true>//tmp/in1', '<teleport=true>//tmp/in2', '//tmp/in3', '//tmp/in4'],
                out = ['<sorted_by=[key]; teleport=true>//tmp/out1', '//tmp/out3'],
                command = 'cat>/dev/fd/4',
                reduce_by = 'key',
                spec={"reducer": {"format": "dsv"}})

        reduce(
            in_ = ["<teleport=true>//tmp/in1", "<teleport=true>//tmp/in2", "//tmp/in3", "//tmp/in4"],
            out = ["<sorted_by=[key]>//tmp/out2", "<sorted_by=[key]; teleport=true>//tmp/out1"],
            command = "cat",
            reduce_by = "key",
            sort_by=["key", "value"],
            spec={"reducer": {"format": "dsv"}})

        assert read_table("//tmp/out1") == \
            [
                {"key": 0, "value": 1},
                {"key": 2, "value": 2},
                {"key": 4, "value": 3},
                {"key": 7, "value": 4}
            ]

        assert read_table("//tmp/out2") == \
            [
                {"key": "8", "value": "1"},
                {"key": "8", "value": "5"},
                {"key": "9", "value": "6"},
                {"key": "9", "value": "7"},
            ]

        assert get("//tmp/out1/@sorted")
        assert get("//tmp/out2/@sorted")

    @authors("psushin", "klyachin")
    @unix_only
    def test_maniac_chunk(self):
        create("table", "//tmp/in1")
        write_table(
            "//tmp/in1",
            [
                {"key": 0, "value": 1},
                {"key": 2, "value": 9}
            ],
            sorted_by = "key")

        create("table", "//tmp/in2")
        write_table(
            "//tmp/in2",
            [
                {"key": 2, "value": 6},
                {"key": 2, "value": 7},
                {"key": 2, "value": 8}
            ],
            sorted_by = "key")

        create("table", "//tmp/out")

        reduce(
            in_ = ["//tmp/in1", "//tmp/in2"],
            out = ["<sorted_by=[key]>//tmp/out"],
            reduce_by="key",
            command = "cat",
            spec={"reducer": {"format": "dsv"}})

        assert read_table("//tmp/out") == \
            [
                {"key": "0", "value": "1"},
                {"key": "2", "value": "9"},
                {"key": "2", "value": "6"},
                {"key": "2", "value": "7"},
                {"key": "2", "value": "8"}
            ]

        assert get("//tmp/out/@sorted")

    @authors("panin", "klyachin")
    def test_empty_in(self):
        create("table", "//tmp/in")

        # TODO(panin): replace it with sort of empty input (when it will be fixed)
        write_table("//tmp/in", {"foo": "bar"}, sorted_by="a")
        erase("//tmp/in")

        create("table", "//tmp/out")

        reduce(
            in_ = "//tmp/in",
            out = "//tmp/out",
            reduce_by="a",
            command = "cat")

        assert read_table("//tmp/out") == []

    @authors("babenko")
    def test_no_outputs(self):
        create("table", "//tmp/t1")
        write_table("<sorted_by=[key]>//tmp/t1", [{"key": "value"}])
        op = reduce(in_="//tmp/t1", command="cat > /dev/null; echo stderr>&2", reduce_by=["key"])
        check_all_stderrs(op, "stderr\n", 1)

    @authors("psushin", "klyachin")
    def test_duplicate_key_columns(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")

        with pytest.raises(YtError):
            reduce(
                in_ = "//tmp/in",
                out = "//tmp/out",
                command = "cat",
                reduce_by=["a", "b", "a"])

    @authors("panin", "klyachin")
    def test_unsorted_input(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", {"foo": "bar"})

        with pytest.raises(YtError):
            reduce(
                in_ = "//tmp/in",
                out = "//tmp/out",
                command = "cat")

    @authors("panin", "klyachin")
    def test_non_prefix(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", {"key": "1", "subkey": "2"}, sorted_by=["key", "subkey"])

        with pytest.raises(YtError):
            reduce(
                in_ = "//tmp/in",
                out = "//tmp/out",
                command = "cat",
                reduce_by="subkey")

    @authors("psushin", "klyachin")
    def test_short_limits(self):
        create("table", "//tmp/in1")
        create("table", "//tmp/in2")
        create("table", "//tmp/out")
        write_table("//tmp/in1", [{"key": "1", "subkey": "2"}, {"key": "2"}], sorted_by=["key", "subkey"])
        write_table("//tmp/in2", [{"key": "1", "subkey": "2"}, {"key": "2"}], sorted_by=["key", "subkey"])

        reduce(
            in_ = ['//tmp/in1["1":"2"]', "//tmp/in2"],
            out = "<sorted_by=[key; subkey]>//tmp/out",
            command = "cat",
            reduce_by=["key", "subkey"],
            spec={"reducer": {"format": yson.loads("<line_prefix=tskv>dsv")},
              "data_size_per_job": 1})

        assert read_table("//tmp/out") == \
            [
                {"key": "1", "subkey": "2"},
                {"key": "1", "subkey": "2"},
                {"key": "2", "subkey" : YsonEntity()}
            ]

        assert get("//tmp/out/@sorted")

    @authors("panin", "klyachin")
    @unix_only
    def test_many_output_tables(self):
        output_tables = ["//tmp/t%d" % i for i in range(3)]

        create("table", "//tmp/t_in")
        for table_path in output_tables:
            create("table", table_path)

        write_table("//tmp/t_in", [{"k": 10}], sorted_by="k")

        reducer = \
"""
cat  > /dev/null
echo {v = 0} >&1
echo {v = 1} >&4
echo {v = 2} >&7
"""
        create("file", "//tmp/reducer.sh")
        write_file("//tmp/reducer.sh", reducer)

        reduce(in_="//tmp/t_in",
            out=output_tables,
            reduce_by="k",
            command="bash reducer.sh",
            file="//tmp/reducer.sh")

        assert read_table(output_tables[0]) == [{"v": 0}]
        assert read_table(output_tables[1]) == [{"v": 1}]
        assert read_table(output_tables[2]) == [{"v": 2}]

    @authors("ignat", "klyachin")
    def test_job_count(self):
        create("table", "//tmp/in", attributes={"compression_codec": "none"})
        create("table", "//tmp/out")

        count = 10000

        # Job count works only if we have enough splits in input chunks.
        # Its default rate 0.0001, so we should have enough rows in input table
        write_table(
            "//tmp/in",
            [ {"key": "%.010d" % num} for num in xrange(count) ],
            sorted_by = ["key"],
            table_writer = {"block_size": 1024})

        reduce(
            in_ = "//tmp/in",
            out = "//tmp/out",
            command = "cat; echo 'key=10'",
            reduce_by=["key"],
            spec={"reducer": {"format": "dsv"},
                  "data_size_per_job": 1})

        # Check that operation has more than 1 job
        assert get("//tmp/out/@row_count") >= count + 2

    @authors("monster")
    def test_key_switch_yamr(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")

        write_table(
            "//tmp/in",
            [
                {"key": "a", "value": ""},
                {"key": "b", "value": ""},
                {"key": "b", "value": ""}
            ],
            sorted_by = ["key"])

        op = reduce(
            in_="//tmp/in",
            out="//tmp/out",
            command="cat 1>&2",
            reduce_by=["key"],
            spec={
                "job_io": {"control_attributes": {"enable_key_switch": "true"}},
                "reducer": {"format": yson.loads("<lenval=true>yamr")},
                "job_count": 1
            })

        jobs_path = op.get_path() + "/jobs"
        job_ids = ls(jobs_path)
        assert len(job_ids) == 1
        stderr_bytes = read_file("{0}/{1}/stderr".format(jobs_path, job_ids[0]))

        assert stderr_bytes.encode("hex") == \
            "010000006100000000" \
            "feffffff" \
            "010000006200000000" \
            "010000006200000000"

        assert not get('//tmp/out/@sorted')

    @authors("monster", "klyachin")
    def test_key_switch_yson(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")

        write_table(
            "//tmp/in",
            [
                {"key": "a", "value": ""},
                {"key": "b", "value": ""},
                {"key": "b", "value": ""}
            ],
            sorted_by = ["key"])

        op = reduce(
            in_="//tmp/in",
            out="//tmp/out",
            command="cat 1>&2",
            reduce_by=["key"],
            spec={
                "job_io": {"control_attributes": {"enable_key_switch": "true"}},
                "reducer": {"format": yson.loads("<format=text>yson")},
                "job_count": 1
            })

        assert not get("//tmp/out/@sorted")

        jobs_path = op.get_path() + "/jobs"
        job_ids = ls(jobs_path)
        assert len(job_ids) == 1
        stderr_bytes = read_file("{0}/{1}/stderr".format(jobs_path, job_ids[0]))

        assert stderr_bytes == \
"""{"key"="a";"value"="";};
<"key_switch"=%true;>#;
{"key"="b";"value"="";};
{"key"="b";"value"="";};
"""

    @authors("klyachin")
    def test_reduce_with_small_block_size(self):
        create("table", "//tmp/in", attributes={"compression_codec": "none"})
        create("table", "//tmp/out")

        count = 300

        write_table(
            "//tmp/in",
            [ {"key": "%05d"%num} for num in xrange(count) ],
            sorted_by = ["key"],
            table_writer = {"block_size": 1024})
        write_table(
            "<append=true>//tmp/in",
            [ {"key": "%05d"%num} for num in xrange(count, 2*count) ],
            sorted_by = ["key"],
            table_writer = {"block_size": 1024})

        reduce(
            in_ = '<ranges=[{lower_limit={row_index=100;key=["00010"]};upper_limit={row_index=540;key=["00560"]}}]>//tmp/in',
            out = "//tmp/out",
            command = "cat",
            reduce_by=["key"],
            spec={"reducer": {"format": "dsv"},
                  "data_size_per_job": 500})

        # Expected the same number of rows in output table
        assert get("//tmp/out/@row_count") == 440

        assert not get("//tmp/out/@sorted")

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_join_one_job(self):
        create("table", "//tmp/hosts")
        write_table(
            "//tmp/hosts",
            [
                {"host": "1", "value":21},
                {"host": "2", "value":22},
                {"host": "3", "value":23},
                {"host": "4", "value":24},
            ],
            sorted_by = ["host"])

        create("table", "//tmp/fresh_hosts")
        write_table(
            "//tmp/fresh_hosts",
            [
                {"host": "2", "value":62},
                {"host": "4", "value":64},
            ],
            sorted_by = ["host"])

        create("table", "//tmp/urls")
        write_table(
            "//tmp/urls",
            [
                {"host":"1", "url":"1/1", "value":11},
                {"host":"1", "url":"1/2", "value":12},
                {"host":"2", "url":"2/1", "value":13},
                {"host":"2", "url":"2/2", "value":14},
                {"host":"3", "url":"3/1", "value":15},
                {"host":"3", "url":"3/2", "value":16},
                {"host":"4", "url":"4/1", "value":17},
                {"host":"4", "url":"4/2", "value":18},
            ],
            sorted_by = ["host", "url"])

        create("table", "//tmp/fresh_urls")
        write_table(
            "//tmp/fresh_urls",
            [
                {"host":"1", "url":"1/2", "value":42},
                {"host":"2", "url":"2/1", "value":43},
                {"host":"3", "url":"3/1", "value":45},
                {"host":"4", "url":"4/2", "value":48},
            ],
            sorted_by = ["host", "url"])

        create("table", "//tmp/output")

        reduce(
            in_ = ["<foreign=true>//tmp/hosts", "<foreign=true>//tmp/fresh_hosts", "//tmp/urls", "//tmp/fresh_urls"],
            out = ["<sorted_by=[host;url]>//tmp/output"],
            command = "cat",
            reduce_by = ["host", "url"],
            join_by = "host",
            spec = {
                "reducer": {
                    "format": yson.loads("<enable_table_index=true>dsv")
                },
                "job_count": 1,
            })

        assert read_table("//tmp/output") == \
            [
                {"host":"1", "url":None,  "value":"21", "@table_index":"0"},
                {"host":"1", "url":"1/1", "value":"11", "@table_index":"2"},
                {"host":"1", "url":"1/2", "value":"12", "@table_index":"2"},
                {"host":"1", "url":"1/2", "value":"42", "@table_index":"3"},
                {"host":"2", "url":None,  "value":"22", "@table_index":"0"},
                {"host":"2", "url":None,  "value":"62", "@table_index":"1"},
                {"host":"2", "url":"2/1", "value":"13", "@table_index":"2"},
                {"host":"2", "url":"2/1", "value":"43", "@table_index":"3"},
                {"host":"2", "url":"2/2", "value":"14", "@table_index":"2"},
                {"host":"3", "url":None,  "value":"23", "@table_index":"0"},
                {"host":"3", "url":"3/1", "value":"15", "@table_index":"2"},
                {"host":"3", "url":"3/1", "value":"45", "@table_index":"3"},
                {"host":"3", "url":"3/2", "value":"16", "@table_index":"2"},
                {"host":"4", "url":None,  "value":"24", "@table_index":"0"},
                {"host":"4", "url":None,  "value":"64", "@table_index":"1"},
                {"host":"4", "url":"4/1", "value":"17", "@table_index":"2"},
                {"host":"4", "url":"4/2", "value":"18", "@table_index":"2"},
                {"host":"4", "url":"4/2", "value":"48", "@table_index":"3"},
            ]

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_skip_joining_rows(self):
        create("table", "//tmp/hosts")
        write_table(
            "//tmp/hosts",
            [{"host": "%d" % i, "value": 20 + i} for i in range(10)],
            sorted_by = ["host"])

        create("table", "//tmp/urls")
        write_table(
            "//tmp/urls",
            [
                {"host":"2", "url":"2/1", "value":11},
                {"host":"2", "url":"2/2", "value":12},
                {"host":"5", "url":"5/1", "value":13},
                {"host":"5", "url":"5/2", "value":14},
            ],
            sorted_by = ["host", "url"])

        create("table", "//tmp/output")

        reduce(
            in_ = ["<foreign=true>//tmp/hosts", "//tmp/urls"],
            out = ["<sorted_by=[host;url]>//tmp/output"],
            command = "cat",
            reduce_by = ["host", "url"],
            join_by = "host",
            spec = {
                "reducer": {
                    "format": yson.loads("<enable_table_index=true>dsv")
                },
                "job_count": 1,
            })

        assert read_table("//tmp/output") == \
            [
                {"host":"2", "url":None,  "value":"22", "@table_index":"0"},
                {"host":"2", "url":"2/1", "value":"11", "@table_index":"1"},
                {"host":"2", "url":"2/2", "value":"12", "@table_index":"1"},
                {"host":"5", "url":None,  "value":"25", "@table_index":"0"},
                {"host":"5", "url":"5/1", "value":"13", "@table_index":"1"},
                {"host":"5", "url":"5/2", "value":"14", "@table_index":"1"},
            ]

    def _prepare_join_tables(self):
        create("table", "//tmp/hosts")
        for i in range(9):
            write_table(
                "<append=true>//tmp/hosts",
                [
                    {"host": str(i), "value":20+2*i},
                    {"host": str(i+1), "value":20+2*i+1},
                ],
                sorted_by = ["host"])

        create("table", "//tmp/fresh_hosts")
        for i in range(0,7,2):
            write_table(
                "<append=true>//tmp/fresh_hosts",
                [
                    {"host": str(i), "value":60+2*i},
                    {"host": str(i+2), "value":60+2*i+1},
                ],
                sorted_by = ["host"])

        create("table", "//tmp/urls")
        for i in range(9):
            for j in range(2):
                    write_table(
                        "<append=true>//tmp/urls",
                        [
                            {"host":str(i), "url":str(i)+"/"+str(j), "value":10+i*2+j},
                        ],
                        sorted_by = ["host", "url"])

        create("table", "//tmp/fresh_urls")
        for i in range(9):
            write_table(
                "<append=true>//tmp/fresh_urls",
                [
                    {"host":str(i), "url":str(i)+"/"+str(i%2), "value":40+i},
                ],
                sorted_by = ["host", "url"])

        create("table", "//tmp/output")

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_join_with_ranges(self):
        self._prepare_join_tables()

        reduce(
            in_ = ["<foreign=true>//tmp/hosts", "<foreign=true>//tmp/fresh_hosts", '//tmp/urls[("3","3/0"):("5")]', '//tmp/fresh_urls[("3","3/0"):("5")]'],
            out = ["<sorted_by=[host;url]>//tmp/output"],
            command = "cat",
            reduce_by = ["host", "url"],
            join_by = "host",
            spec = {
                "reducer": {
                    "format": yson.loads("<enable_table_index=true>dsv")
                },
                "job_count": 1,
            })

        assert read_table("//tmp/output") == \
            [
                {"host":"3", "url":None,  "value":"25", "@table_index":"0"},
                {"host":"3", "url":None,  "value":"26", "@table_index":"0"},
                {"host":"3", "url":"3/0", "value":"16", "@table_index":"2"},
                {"host":"3", "url":"3/1", "value":"17", "@table_index":"2"},
                {"host":"3", "url":"3/1", "value":"43", "@table_index":"3"},
                {"host":"4", "url":None,  "value":"27", "@table_index":"0"},
                {"host":"4", "url":None,  "value":"28", "@table_index":"0"},
                {"host":"4", "url":None,  "value":"65", "@table_index":"1"},
                {"host":"4", "url":None,  "value":"68", "@table_index":"1"},
                {"host":"4", "url":"4/0", "value":"18", "@table_index":"2"},
                {"host":"4", "url":"4/0", "value":"44", "@table_index":"3"},
                {"host":"4", "url":"4/1", "value":"19", "@table_index":"2"},
            ]

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_join_multiple_jobs(self):
        self._prepare_join_tables()

        reduce(
            in_ = ["<foreign=true>//tmp/hosts", "<foreign=true>//tmp/fresh_hosts", '//tmp/urls[("3","3/0"):("5")]', '//tmp/fresh_urls[("3","3/0"):("5")]'],
            out = ["//tmp/output"],
            command = "cat",
            reduce_by = ["host", "url"],
            join_by = "host",
            spec = {
                "reducer": {
                    "format": yson.loads("<enable_table_index=true>dsv")
                },
                "data_size_per_job": 1,
            })

        assert len(read_table("//tmp/output")) == 18

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_multiple_jobs(self):
        for i in range(4):
            create("table", "//tmp/t{0}".format(i))
            write_table(
                "//tmp/t" + str(i),
                [{"key": "%05d" % j, "value": "%05d" % j} for j in range(20-i, 30+i)],
                sorted_by = ["key","value"])

        create("table", "//tmp/foreign")
        write_table(
            "//tmp/foreign",
            [{"key": "%05d" % i, "value": "%05d" % (10000+i)} for i in range(50)],
            sorted_by = ["key"])

        create("table", "//tmp/output")

        reduce(
            in_ = ["<foreign=true>//tmp/foreign"] + ["//tmp/t{0}".format(i) for i in range(4)],
            out = ["<sorted_by=[key]>//tmp/output"],
            command = "grep @table_index=0 | head -n 1",
            reduce_by = ["key","value"],
            join_by = ["key"],
            spec = {
                "reducer": {
                    "format": yson.loads("<enable_table_index=true>dsv")
                },
                "job_count": 5,
            })

        output = read_table("//tmp/output")
        assert len(output) > 1
        assert output[0] == {"key":"00017", "value":"10017", "@table_index":"0"}

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_reduce_by_equals_join_by(self):
        self._prepare_join_tables()

        reduce(
            in_ = ["<foreign=true>//tmp/hosts", "<foreign=true>//tmp/fresh_hosts", '//tmp/urls[("3","3/0"):("5")]', '//tmp/fresh_urls[("3","3/0"):("5")]'],
            out = ["//tmp/output"],
            command = "cat",
            reduce_by = "host",
            join_by = "host",
            spec = {
                "reducer": {
                    "format": yson.loads("<enable_table_index=true>dsv")
                },
                "job_count": 1,
            })

        assert len(read_table("//tmp/output")) == 12

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_invalid_reduce_by(self):
        self._prepare_join_tables()

        with pytest.raises(YtError):
            reduce(
                in_ = ["<foreign=true>//tmp/urls", "//tmp/fresh_urls"],
                out = ["//tmp/output"],
                command = "cat",
                reduce_by = ["host"],
                join_by = ["host", "url"],
                spec = {
                    "reducer": {
                        "format": yson.loads("<enable_table_index=true>dsv")
                    },
                    "job_count": 1,
                })

    @authors("klyachin")
    @unix_only
    def test_reduce_with_foreign_join_key_switch_yson(self):
        create("table", "//tmp/hosts")
        write_table(
            "//tmp/hosts",
            [
                {"key": "1", "value":"21"},
                {"key": "2", "value":"22"},
                {"key": "3", "value":"23"},
                {"key": "4", "value":"24"},
            ],
            sorted_by = ["key"])

        create("table", "//tmp/urls")
        write_table(
            "//tmp/urls",
            [
                {"key":"1", "subkey":"1/1", "value":"11"},
                {"key":"1", "subkey":"1/2", "value":"12"},
                {"key":"2", "subkey":"2/1", "value":"13"},
                {"key":"2", "subkey":"2/2", "value":"14"},
                {"key":"3", "subkey":"3/1", "value":"15"},
                {"key":"3", "subkey":"3/2", "value":"16"},
                {"key":"4", "subkey":"4/1", "value":"17"},
                {"key":"4", "subkey":"4/2", "value":"18"},
            ],
            sorted_by = ["key", "subkey"])

        create("table", "//tmp/output")

        op = reduce(
            in_ = ["<foreign=true>//tmp/hosts", "//tmp/urls"],
            out = "//tmp/output",
            command = "cat 1>&2",
            reduce_by = ["key", "subkey"],
            join_by = ["key"],
            spec = {
                "job_io": {
                    "control_attributes": {
                        "enable_key_switch": "true"
                    }
                },
                "reducer": {
                    "format": yson.loads("<format=text>yson"),
                    "enable_input_table_index": True
                },
                "job_count": 1
            })

        jobs_path = op.get_path() + "/jobs"
        job_ids = ls(jobs_path)
        assert len(job_ids) == 1
        stderr_bytes = read_file("{0}/{1}/stderr".format(jobs_path, job_ids[0]))

        assert stderr_bytes == \
"""<"table_index"=0;>#;
{"key"="1";"value"="21";};
<"table_index"=1;>#;
{"key"="1";"subkey"="1/1";"value"="11";};
{"key"="1";"subkey"="1/2";"value"="12";};
<"key_switch"=%true;>#;
<"table_index"=0;>#;
{"key"="2";"value"="22";};
<"table_index"=1;>#;
{"key"="2";"subkey"="2/1";"value"="13";};
{"key"="2";"subkey"="2/2";"value"="14";};
<"key_switch"=%true;>#;
<"table_index"=0;>#;
{"key"="3";"value"="23";};
<"table_index"=1;>#;
{"key"="3";"subkey"="3/1";"value"="15";};
{"key"="3";"subkey"="3/2";"value"="16";};
<"key_switch"=%true;>#;
<"table_index"=0;>#;
{"key"="4";"value"="24";};
<"table_index"=1;>#;
{"key"="4";"subkey"="4/1";"value"="17";};
{"key"="4";"subkey"="4/2";"value"="18";};
"""

    @authors("klyachin")
    @unix_only
    def test_reduce_row_count_limit(self):
        create("table", "//tmp/input")
        for i in xrange(5):
            write_table(
                "<append=true>//tmp/input",
                [{"key": "%05d"%i, "value": "foo"}],
                sorted_by = ["key"])

        create("table", "//tmp/output")
        op = reduce(
            in_="//tmp/input",
            out="<row_count_limit=3>//tmp/output",
            command="cat",
            reduce_by=["key"],
            spec={
                "reducer": {
                    "format": "dsv"
                },
                "data_size_per_job": 1,
                "max_failed_job_count": 1
            })

        assert len(read_table("//tmp/output")) == 3

    @authors("dakovalkov")
    @unix_only
    def test_reduce_row_count_limit_teleport(self):
        create("table", "//tmp/in1", attributes = {
            "schema": make_schema([
                {"name": "key", "type": "int64", "sort_order": "ascending"}])
            })
        create("table", "//tmp/in2", attributes = {
            "schema": make_schema([
                {"name": "key", "type": "int64", "sort_order": "ascending"}])
            })
        write_table("//tmp/in1", [{"key": 1}, {"key" : 3}])
        write_table("<append=true>//tmp/in1", [{"key": 5}, {"key" : 7}])
        write_table("//tmp/in2", [{"key": 6}, {"key" : 10}, {"key" : 11}])
        create("table", "//tmp/out")

        reduce(
            in_=["<teleport=true>//tmp/in1", "<teleport=true>//tmp/in2"],
            out="<teleport=true;row_count_limit=1>//tmp/out",
            command = "cat",
            reduce_by=["key"],
            spec={
                "reducer": {
                    "format": "dsv"
                }
            })

        assert read_table("//tmp/out") == [{"key": 1}, {"key": 3}]

    @authors("dakovalkov")
    @unix_only
    def test_reduce_row_count_limit_teleport_2(self):
        create("table", "//tmp/in1", attributes = {
            "schema": make_schema([
                {"name": "key", "type": "int64", "sort_order": "ascending"}])
            })
        create("table", "//tmp/in2", attributes = {
            "schema": make_schema([
                {"name": "key", "type": "int64", "sort_order": "ascending"}])
            })
        write_table("//tmp/in1", [{"key": 1}, {"key" : 3}])
        write_table("<append=true>//tmp/in1", [{"key": 5}, {"key" : 7}])
        write_table("//tmp/in2", [{"key": 6}, {"key" : 10}, {"key" : 11}])
        create("table", "//tmp/out1")
        create("table", "//tmp/out2")

        reduce(
            in_=["<teleport=true>//tmp/in1", "<teleport=true>//tmp/in2"],
            out=["<row_count_limit=1>//tmp/out1", "<teleport=true>//tmp/out2"],
            command = "cat",
            reduce_by=["key"])

        assert sorted(read_table("//tmp/out1")) == [{"key": 5}, {"key" : 7}]
        assert sorted(read_table("//tmp/out2")) == [{"key": 1}, {"key" : 3}, {"key": 6}, {"key" : 10}, {"key" : 11}]

    @authors("savrus")
    @unix_only
    @pytest.mark.parametrize("sort_order", [None, "ascending"])
    def test_schema_validation(self, sort_order):
        create("table", "//tmp/input")
        create("table", "//tmp/output", attributes={
            "schema": make_schema([
                {"name": "key", "type": "int64", "sort_order": sort_order},
                {"name": "value", "type": "string"}])
            })

        for i in xrange(10):
            write_table("<append=true; sorted_by=[key]>//tmp/input", {"key": i, "value": "foo"})
            print_debug(get("//tmp/input/@schema"))

        reduce(
            in_="//tmp/input",
            out="//tmp/output",
            reduce_by="key",
            command="cat")

        assert get("//tmp/output/@schema_mode") == "strong"
        assert get("//tmp/output/@schema/@strict")
        assert_items_equal(read_table("//tmp/output"), [{"key": i, "value": "foo"} for i in xrange(10)])

        write_table("<sorted_by=[key]>//tmp/input", {"key": "1", "value": "foo"})
        assert get("//tmp/input/@sorted_by") == ["key"]

        with pytest.raises(YtError):
            reduce(
                in_="//tmp/input",
                out="//tmp/output",
                reduce_by="key",
                command="cat")

    @authors("babenko", "klyachin")
    @unix_only
    def test_reduce_input_paths_attr(self):
        create("table", "//tmp/in1")
        for i in xrange(0, 5, 2):
            write_table(
                "<append=true>//tmp/in1",
                [{"key": "%05d" % (i+j), "value": "foo"} for j in xrange(2)],
                sorted_by = ["key"])

        create("table", "//tmp/in2")
        for i in xrange(3, 16, 2):
            write_table(
                "<append=true>//tmp/in2",
                [{"key": "%05d" % ((i+j)/4), "value": "foo"} for j in xrange(2)],
                sorted_by = ["key", "value"])

        create("table", "//tmp/out")
        op = reduce(
            track=False,
            in_=["<foreign=true>//tmp/in1", '//tmp/in2["00001":"00004"]'],
            out="//tmp/out",
            command="exit 1",
            reduce_by=["key", "value"],
            join_by=["key"],
            spec={
                "reducer": {
                    "format": "dsv"
                },
                "job_count": 1,
                "max_failed_job_count": 1
            })
        with pytest.raises(YtError):
            op.track();

    @authors("savrus")
    @unix_only
    def test_computed_columns(self):
        create("table", "//tmp/t1")
        create("table", "//tmp/t2",
            attributes={
                "schema": [
                    {"name": "k1", "type": "int64", "expression": "k2 * 2" },
                    {"name": "k2", "type": "int64"}]
            })

        write_table("<sorted_by=[k2]>//tmp/t1", [{"k2": i} for i in xrange(2)])

        reduce(
            in_="//tmp/t1",
            out="//tmp/t2",
            reduce_by="k2",
            command="cat")

        assert get("//tmp/t2/@schema_mode") == "strong"
        assert read_table("//tmp/t2") == [{"k1": i * 2, "k2": i} for i in xrange(2)]

    @authors("savrus")
    @unix_only
    @parametrize_external
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_reduce_on_dynamic_table(self, optimize_for, external):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t", optimize_for=optimize_for, external=external)
        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(10)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)
        sync_unmount_table("//tmp/t")

        reduce(
            in_="//tmp/t",
            out="//tmp/t_out",
            reduce_by="key",
            command="cat")

        assert_items_equal(read_table("//tmp/t_out"), rows)

        rows = [{"key": i, "value": str(i+1)} for i in range(10)]
        sync_mount_table("//tmp/t")
        insert_rows("//tmp/t", rows)
        sync_unmount_table("//tmp/t")

        reduce(
            in_="//tmp/t",
            out="//tmp/t_out",
            reduce_by="key",
            command="cat")

        assert_items_equal(read_table("//tmp/t_out"), rows)

    @authors("savrus")
    @unix_only
    @parametrize_external
    @pytest.mark.parametrize("optimize_for", ["lookup", "scan"])
    def test_reduce_with_foreign_dynamic(self, optimize_for, external):
        sync_create_cells(1)
        self._create_simple_dynamic_table("//tmp/t2", optimize_for=optimize_for, external=external)
        create("table", "//tmp/t1")
        create("table", "//tmp/t_out")

        rows = [{"key": i, "value": str(i)} for i in range(10)]
        sync_mount_table("//tmp/t2")
        insert_rows("//tmp/t2", rows)
        sync_unmount_table("//tmp/t2")

        write_table("<sorted_by=[key]>//tmp/t1", [{"key": i} for i in (8, 9)])

        reduce(
            in_=["//tmp/t1", "<foreign=true>//tmp/t2"],
            out="//tmp/t_out",
            reduce_by="key",
            join_by="key",
            command="cat",
            spec={
                "reducer": {
                   "format": yson.loads("<enable_table_index=true>dsv")
                }
            })

        expected = [{"key": str(i), "@table_index": "0"} for i in (8, 9)] + \
            [{"key": str(i), "value": str(i), "@table_index": "1"} for i in (8, 9)]

        assert_items_equal(read_table("//tmp/t_out"), expected)

    @authors("savrus")
    @parametrize_external
    def test_dynamic_table_index(self, external):
        sync_create_cells(1)
        create("table", "//tmp/t1")
        self._create_simple_dynamic_table("//tmp/t2", external=external)
        self._create_simple_dynamic_table("//tmp/t3", external=external)
        create("table", "//tmp/t_out")

        sync_mount_table("//tmp/t2")
        sync_mount_table("//tmp/t3")

        write_table("<sorted_by=[key]>//tmp/t1", [{"key": i, "value": str(i)} for i in range(1)])
        insert_rows("//tmp/t2", [{"key": i, "value": str(i)} for i in range(1, 2)])
        insert_rows("//tmp/t3", [{"key": i, "value": str(i)} for i in range(2, 3)])

        sync_flush_table("//tmp/t2")
        sync_flush_table("//tmp/t3")

        op = reduce(in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
            out="//tmp/t_out",
            command="cat > /dev/stderr",
            reduce_by = "key",
            spec={"reducer": {
                    "enable_input_table_index": True,
                    "format": yson.loads("<format=text>yson"),
                }})

        job_ids = ls(op.get_path() + "/jobs")
        assert len(job_ids) == 1
        output = read_file(op.get_path() + "/jobs/{}/stderr".format(job_ids[0]))
        assert output == \
"""<"table_index"=0;>#;
{"key"=0;"value"="0";};
<"table_index"=1;>#;
{"key"=1;"value"="1";};
<"table_index"=2;>#;
{"key"=2;"value"="2";};
"""

    @authors("klyachin")
    @unix_only
    @pytest.mark.parametrize("with_foreign", [False, True])
    def test_reduce_interrupt_job(self, with_foreign):
        if with_foreign:
            in_=["<foreign=true>//tmp/input2", "//tmp/input1"],
            kwargs={"join_by": ["key"]}
        else:
            in_=["//tmp/input1"],
            kwargs={}

        create("table", "//tmp/input1")
        write_table(
            "//tmp/input1",
            [{"key": "(%08d)" % (i * 2 + 1), "value": "(t_1)", "data": "a" * (2 * 1024 * 1024)} for i in range(3)],
            sorted_by = ["key", "value"])

        create("table", "//tmp/input2")
        write_table(
            "//tmp/input2",
            [{"key": "(%08d)" % (i / 2), "value": "(t_2)"} for i in range(30)],
            sorted_by = ["key"])

        create("table", "//tmp/output")

        op = reduce(
            track=False,
            label="interrupt_job",
            in_=in_,
            out="<sorted_by=[key]>//tmp/output",
            command=with_breakpoint("""read; echo "${REPLY/(???)/(job)}" ; echo "$REPLY" ; BREAKPOINT ; cat"""),
            reduce_by=["key", "value"],
            spec={
                "reducer": {
                    "format": "dsv",
                },
                "max_failed_job_count" : 1,
                "job_io" : {
                    "buffer_row_count" : 1,
                },
                "data_size_per_job" : 256 * 1024 * 1024,
                "enable_job_splitting": False,
            },
            **kwargs)

        jobs = wait_breakpoint()
        interrupt_job(jobs[0], interrupt_timeout=2000000)
        release_breakpoint()

        op.track()

        result = read_table("//tmp/output", verbose=False)
        if with_foreign:
            assert len(result) == 11
        else:
            assert len(result) == 5
        row_index = 0
        job_indexes = []
        row_table_count = {}

        assert get(op.get_path() + "/@progress/jobs/pending") == 0

        for row in result:
            if row["value"] == "(job)":
                job_indexes.append(row_index)
            row_table_count[row["value"]] = row_table_count.get(row["value"], 0) + 1
            row_index += 1
        assert row_table_count["(job)"] == 2
        assert row_table_count["(t_1)"] == 3
        if with_foreign:
            assert row_table_count["(t_2)"] == 6
            assert job_indexes[1] == 4
        else:
            assert job_indexes[1] == 3
        assert get(op.get_path() + "/@progress/job_statistics/data/input/row_count/$/completed/sorted_reduce/sum") == len(result) - 2

    @authors("savrus")
    def test_query_filtering(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "a", "type": "int64"}]
        })
        create("table", "//tmp/t2")
        write_table("//tmp/t1", [{"a": i} for i in xrange(2)])

        with pytest.raises(YtError):
            reduce(
                in_="//tmp/t1",
                out="//tmp/t2",
                command="cat",
                spec={"input_query": "a where a > 0"})

    @authors("klyachin")
    def test_reduce_job_splitter(self):
        create("table", "//tmp/in_1")
        for j in range(5):
            write_table(
                "<append=true>//tmp/in_1",
                [{"key": "%08d" % (j * 4 + i), "value": "(t_1)", "data": "a" * (1024 * 1024)} for i in range(4)],
                sorted_by = ["key", "value"],
                table_writer = {
                    "block_size": 1024,
                })

        create("table", "//tmp/in_2")
        write_table(
            "//tmp/in_2",
            [{"key": "(%08d)" % (i / 2), "value": "(t_2)"} for i in range(40)],
            sorted_by = ["key"])

        input_ = ["<foreign=true>//tmp/in_2"] + ["//tmp/in_1"] * 5
        output = "//tmp/output"
        create("table", output)

        command="""
while read ROW; do
    if [ "$YT_JOB_INDEX" == 0 ]; then
        sleep 2
    else
        sleep 0.2
    fi
    echo "$ROW"
done
"""

        op = reduce(
            track=False,
            label="split_job",
            in_=input_,
            out=output,
            command=command,
            reduce_by=["key", "value"],
            join_by="key",
            spec={
                "reducer": {
                    "format": "dsv",
                },
                "data_size_per_job": 21 * 1024 * 1024,
                "max_failed_job_count": 1,
                "job_io": {
                    "buffer_row_count" : 1,
                },
            })

        op.track()

        completed = get(op.get_path() + "/@progress/jobs/completed")
        interrupted = completed["interrupted"]
        assert completed["total"] >= 6
        assert interrupted["job_split"] >= 1

    @authors("max42")
    def test_intermediate_live_preview(self):
        create("table", "//tmp/t1", attributes={"schema": [{"name": "foo", "type": "string", "sort_order": "ascending"}]})
        write_table("//tmp/t1", {"foo": "bar"})
        create("table", "//tmp/t2")

        op = reduce(track=False, command="cat; sleep 3",
                    in_="//tmp/t1", out="//tmp/t2",
                    reduce_by=["foo"])

        time.sleep(2)

        operation_path = op.get_path()
        scheduler_transaction_id = get(operation_path + "/@async_scheduler_transaction_id")
        wait(lambda: exists(operation_path + "/output_0", tx=scheduler_transaction_id))

        op.track()
        assert read_table("//tmp/t2") == [{"foo": "bar"}]

    @authors("max42")
    def test_pivot_keys(self):
        create("table", "//tmp/t1", attributes={"schema": [
            {"name": "key", "type": "string", "sort_order": "ascending"},
            {"name": "value", "type": "int64"}]})
        create("table", "//tmp/t2")
        for i in range(1, 13):
            write_table("<append=%true>//tmp/t1", {"key": "%02d" % i, "value": i})
        reduce(in_="//tmp/t1",
               out="//tmp/t2",
               command="cat",
               reduce_by=["key"],
               spec={"pivot_keys": [["05"], ["10"]]})
        assert get("//tmp/t2/@chunk_count") == 3
        chunk_ids = get("//tmp/t2/@chunk_ids")
        assert sorted([get("#" + chunk_id + "/@row_count") for chunk_id in chunk_ids]) == [3, 4, 5]

    @authors("max42")
    def test_pivot_keys_incorrect_options(self):
        create("table", "//tmp/t1", attributes={"schema": [
            {"name": "key", "type": "string", "sort_order": "ascending"},
            {"name": "value", "type": "int64"}]})
        create("table", "//tmp/t2")
        for i in range(1, 13):
            write_table("<append=%true>//tmp/t1", {"key": "%02d" % i, "value": i})
        with pytest.raises(YtError):
            reduce(in_="//tmp/t1",
                   out="//tmp/t2",
                   command="cat",
                   reduce_by=["key"],
                   spec={"pivot_keys": [["10"], ["05"]]})
        with pytest.raises(YtError):
            reduce(in_="<teleport=%true>//tmp/t1",
                   out="//tmp/t2",
                   command="cat",
                   reduce_by=["key"],
                   spec={"pivot_keys": [["05"], ["10"]]})

    @authors("max42")
    def test_sampling(self):
        create("table", "//tmp/t1", attributes={"schema": [{"name": "key", "type": "string", "sort_order": "ascending"},
                                                           {"name": "value", "type": "string"}]})
        create("table", "//tmp/t2")
        write_table("//tmp/t1",
                    [{"key": ("%02d" % (i // 100)), "value": "x" * 10**2} for i in range(10000)],
                    table_writer={"block_size": 1024})

        reduce(in_="//tmp/t1",
               out="//tmp/t2",
               command="cat",
               reduce_by=["key"],
               spec={"sampling": {"sampling_rate": 0.5, "io_block_size": 10**5}})
        assert 0.25 * 10000 <= get("//tmp/t2/@row_count") <= 0.75 * 10000
        assert get("//tmp/t2/@chunk_count") == 1

        reduce(in_="//tmp/t1",
               out="//tmp/t2",
               command="cat",
               reduce_by=["key"],
               spec={"sampling": {"sampling_rate": 0.5, "io_block_size": 10**5},
                     "job_count": 10})
        assert 0.25 * 10000 <= get("//tmp/t2/@row_count") <= 0.75 * 10000
        assert get("//tmp/t2/@chunk_count") > 1

        reduce(in_="//tmp/t1",
               out="//tmp/t2",
               command="cat",
               reduce_by=["key"],
               spec={"sampling": {"sampling_rate": 0.5, "io_block_size": 10**7},
                     "job_count": 10})
        assert get("//tmp/t2/@row_count") in [0, 10000]
        assert get("//tmp/t2/@chunk_count") in [0, 1]

        reduce(in_="//tmp/t1",
               out="//tmp/t2",
               command="cat",
               reduce_by=["key"],
               spec={"sampling": {"sampling_rate": 0.5, "io_block_size": 1, "max_total_slice_count": 1},
                     "job_count": 10})
        assert get("//tmp/t2/@row_count") in [0, 10000]
        assert get("//tmp/t2/@chunk_count") in [0, 1]

    @authors("renadeen")
    def test_reduce_skewed_key_distribution_one_table(self):
        create("table", "//tmp/in1")
        create("table", "//tmp/out")

        data = [{"key": "a"}] * 1000 + [{"key": "b"}] * 1
        write_table("//tmp/in1", data, sorted_by=["key"], table_writer={"block_size": 1024})

        reduce(
            in_=["//tmp/in1"],
            out=["//tmp/out"],
            command="uniq",
            reduce_by="key",
            spec={
                "reducer": {"format": yson.loads("dsv")},
                "job_count": 2,
                "enable_key_guarantee": False
            }
        )

        assert get("//tmp/out/@chunk_count") == 2
        expected = [{"key": "a"},
                    {"key": "a"},
                    {"key": "b"}]
        assert sorted(list(read_table("//tmp/out"))) == sorted(expected)

    @authors("renadeen")
    def test_reduce_skewed_key_distribution_two_tables(self):
        create("table", "//tmp/in1")
        create("table", "//tmp/out")

        data = [{"key": "a"}] * 1000 + [{"key": "b"}] * 1
        write_table("//tmp/in1", data, sorted_by=["key"], table_writer={"block_size": 1024})

        reduce(
            in_=["//tmp/in1", "//tmp/in1"],
            out=["//tmp/out"],
            command="uniq -c | awk -v OFS='\t' '{print $2, $1}'",
            reduce_by="key",
            spec={
                "reducer": {
                    "input_format": yson.loads("<columns=[key]>schemaful_dsv"),
                    "output_format": yson.loads("<columns=[key;count]>schemaful_dsv")
                },
                "job_count": 4,
                "enable_key_guarantee": False
            }
        )
        rows = sorted(read_table("//tmp/out"), key=lambda d: d["key"])
        assert len(rows) > 2
        dct = {}
        for key, group in itertools.groupby(rows, key=lambda d: d["key"]):
            dct[key] = sum(int(x["count"]) for x in group)

        assert len(dct) == 2
        assert dct["a"] == 2000
        assert dct["b"] == 2

    @authors("dakovalkov")
    def test_reduce_different_types(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t2", attributes={
            "schema": [{"name": "key", "type": "string", "sort_order": "ascending"}]
            })
        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 1}])
        write_table("//tmp/t2", [{"key": "1"}])

        with pytest.raises(YtError):
            reduce(
                in_=["//tmp/t1", "//tmp/t2"],
                out=["//tmp/out"],
                command="echo {key=5}",
                reduce_by="key")

    @authors("dakovalkov")
    def test_reduce_with_any(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t2", attributes={
            "schema": [{"name": "key", "type": "any", "sort_order": "ascending"}]
            })
        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 1}])
        write_table("//tmp/t2", [{"key": 1}])

        reduce(
            in_=["//tmp/t1", "//tmp/t2"],
            out=["//tmp/out"],
            command="echo {key=5}",
            reduce_by="key")

        assert read_table("//tmp/out") == [{"key": 5}]

    @authors("dakovalkov")
    def test_reduce_different_types_with_any(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "key", "type": "any", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t2", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t3", attributes={
            "schema": [{"name": "key", "type": "string", "sort_order": "ascending"}]
            })
        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 1}])
        write_table("//tmp/t2", [{"key": 1}])
        write_table("//tmp/t3", [{"key": "1"}])

        with pytest.raises(YtError):
            reduce(
                in_=["//tmp/t1", "//tmp/t2", "//tmp/t3"],
                out=["//tmp/out"],
                command="echo {key=5}",
                reduce_by="key")

    @authors("dakovalkov")
    def test_reduce_with_foreign_table(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "key", "type": "any", "sort_order": "ascending"},
                       {"name": "subkey", "type": "int64", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t2", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"},
                       {"name": "subkey", "type": "string"}]
            })

        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 1, "subkey": 2}])
        write_table("//tmp/t2", [{"key": 1, "subkey": "2"}])

        reduce(
            in_=["//tmp/t1", "<foreign=%true>//tmp/t2"],
            out=["//tmp/out"],
            command="echo {key=5}",
            reduce_by=["key", "subkey"],
            join_by="key")

        assert read_table("//tmp/out") == [{"key": 5}]

    @authors("dakovalkov")
    def test_reduce_with_foreign_table_fail(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "key", "type": "any", "sort_order": "ascending"},
                       {"name": "subkey", "type": "int64", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t2", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"},
                       {"name": "subkey", "type": "string", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t3", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"},
                       {"name": "subkey", "type": "int64"}]
            })
        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 1, "subkey": 2}])
        write_table("//tmp/t2", [{"key": 1, "subkey": "3"}])
        write_table("//tmp/t3", [{"key": 1, "subkey": 4}])

        with pytest.raises(YtError):
            reduce(
                in_=["//tmp/t1", "//tmp/t2", "<foreign=%true>//tmp/t3"],
                out=["//tmp/out"],
                command="echo {key=5}",
                reduce_by=["key", "subkey"],
                join_by="key")

    @authors("dakovalkov")
    def test_reduce_different_logical_types(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "key", "type": "int32", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t2", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]
            })
        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 5}])
        write_table("//tmp/t2", [{"key": 5}])

        reduce(
            in_=["//tmp/t1", "//tmp/t2"],
            out=["//tmp/out"],
            command="echo {key=1}",
            reduce_by=["key"])

        assert read_table("//tmp/out") == [{"key": 1}]

    @authors("dakovalkov")
    def test_reduce_disable_check_key_column_types(self):
        create("table", "//tmp/t1", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]
            })
        create("table", "//tmp/t2", attributes={
            "schema": [{"name": "key", "type": "string", "sort_order": "ascending"}]
            })
        create("table", "//tmp/out")

        write_table("//tmp/t1", [{"key": 5}])
        write_table("//tmp/t2", [{"key": "6"}])

        reduce(
            in_=["//tmp/t1", "//tmp/t2"],
            out=["//tmp/out"],
            command="echo {key=1}",
            reduce_by=["key"],
            spec={"validate_key_column_types": False, "job_count": 2})

        assert read_table("//tmp/out") == [{"key": 1}, {"key": 1}]

    @authors("max42")
    def test_reduce_empty_table(self):
        # YT-11740.
        create("table", "//tmp/t_in", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]
        })
        create("table", "//tmp/t_out", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"}]
        })
        reduce(in_=["//tmp/t_in"],
               out=["//tmp/t_out"],
               command="cat >/dev/null",
               reduce_by=["key"],
               spec={"job_count": 1})
        assert get("//tmp/t_out/@row_count") == 0

    @authors("gritukan")
    def test_reduce_without_foreign_tables_and_key_guarantee(self):
        create("table", "//tmp/in1", attributes={
            "schema": [{"name": "key", "type": "string", "sort_order": "ascending"}]
        })
        create("table", "//tmp/in2", attributes={
            "schema": [{"name": "key", "type": "string", "sort_order": "ascending"}]
        })
        create("table", "//tmp/out1")
        create("table", "//tmp/out2")

        write_table("//tmp/in1", [{"key": "1"}, {"key": "3"}])
        write_table("//tmp/in2", [{"key": "2"}, {"key": "4"}])

        reduce(in_=["//tmp/in1", "//tmp/in2"],
               out=["//tmp/out1"],
               command="cat",
               reduce_by=["key"],
               spec={"reducer": {"format": "dsv"}, "enable_key_guarantee": False})
        assert read_table("//tmp/out1") == [{"key": str(i)} for i in range(1, 5)]

        reduce(in_=["<primary=%true>//tmp/in1", "<primary=%true>//tmp/in2"],
               out=["//tmp/out2"],
               command="cat",
               reduce_by=["key"],
               spec={"reducer": {"format": "dsv"}, "enable_key_guarantee": False})
        assert read_table("//tmp/out2") == [{"key": str(i)} for i in range(1, 5)]

    @authors("gritukan")
    def test_sort_by_without_key_guarantee(self):
        create("table", "//tmp/in1", attributes = {
            "schema": make_schema([
                {"name": "k1", "type": "string", "sort_order": "ascending"},
                {"name": "k2", "type": "string", "sort_order": "ascending"}])
            })
        create("table", "//tmp/in2", attributes = {
            "schema": make_schema([
                {"name": "k1", "type": "string", "sort_order": "ascending"},
                {"name": "k2", "type": "string", "sort_order": "ascending"}])
            })
        create("table", "//tmp/f", attributes = {
            "schema": make_schema([
                {"name": "k1", "type": "string", "sort_order": "ascending"},
                {"name": "k2", "type": "string", "sort_order": "ascending"}])
            })
        create("table", "//tmp/out", attributes = {
            "schema": make_schema([
                {"name": "k1", "type": "string", "sort_order": "ascending"},
                {"name": "k2", "type": "string", "sort_order": "ascending"}])
            })

        write_table("//tmp/in1", [{"k1": "1", "k2": "1"}, {"k1": "1", "k2": "3"}])
        write_table("//tmp/in2", [{"k1": "1", "k2": "2"}, {"k1": "1", "k2": "4"}])
        write_table("//tmp/f", [{"k1": "1", "k2": "0"}])

        reduce(
            in_=["//tmp/in1", "//tmp/in2", "<foreign=true>//tmp/f"],
            out="//tmp/out",
            command = "cat | grep -v 0", # Skip foreign row.
            sort_by=["k1", "k2"],
            join_by=["k1"],
            spec={
                "reducer": {
                    "format": "dsv"
                },
                "enable_key_guarantee": False
            })

        assert read_table("//tmp/out") == [{"k1": "1", "k2": str(i)} for i in range(1, 5)]

        with pytest.raises(YtError):
            reduce(
                in_=["//tmp/in1", "//tmp/in2", "<foreign=true>//tmp/f"],
                out="//tmp/out",
                command = "cat | grep -v 0", # Skip foreign row.
                sort_by=["k1"],
                join_by=["k1", "k2"],
                spec={
                    "reducer": {
                        "format": "dsv"
                    },
                    "enable_key_guarantee": False
                })

    @authors("ermolovd")
    def test_complex_key_reduce(self):
        create("table", "//tmp/in", attributes={
            "schema": [
                {"name": "key", "type_v3": tuple_type(["int64", "int64"]), "sort_order": "ascending"},
                {"name": "value", "type_v3": "int64"},
            ],
            "compression_codec": "none"
        })

        write_table("//tmp/in", [
            {"key": (1, 2), "value": 1},
            {"key": (1, 2), "value": 5},
            {"key": (2, 3), "value": 8},
            {"key": (2, 3), "value": -1},
        ])

        create("table", "//tmp/out")

        script = """
import json
import sys

key_start = True
for line in sys.stdin:
    row = json.loads(line)
    if "$attributes" in row:
        if row["$attributes"].get("key_switch", False):
            key_start = True
    else:
        row["key_start"] = key_start
        print(json.dumps(row))
        key_start=False
        """

        create("file", "//tmp/script.py")
        write_file("//tmp/script.py", script)

        op = reduce(
            in_="//tmp/in",
            out="//tmp/out",
            reduce_by="key",
            command="python script.py",
            file="//tmp/script.py",
            spec={
                "reducer": {
                    "format": "json"
                },
                "job_io": {"control_attributes": {"enable_key_switch": "true"}},
            })
        op.track()
        assert read_table("//tmp/out") == [
            {"key": [1, 2], "value": 1, "key_start": True},
            {"key": [1, 2], "value": 5, "key_start": False},
            {"key": [2, 3], "value": 8, "key_start": True},
            {"key": [2, 3], "value": -1, "key_start": False},
        ]

    @authors("gritukan")
    def test_teleport_and_foreign(self):
        create("table", "//tmp/in_t1", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"},
                       {"name": "value", "type": "int64"}]})
        write_table(
            "//tmp/in_t1",
            [
                {"key": 0, "value": 0},
                {"key": 1, "value": 1},
            ])

        create("table", "//tmp/in_t2", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"},
                       {"name": "value", "type": "int64"}]})
        write_table(
            "//tmp/in_t2",
            [
                {"key": 0, "value": 0},
            ])

        create("table", "//tmp/in_f", attributes={
            "schema": [{"name": "key", "type": "int64", "sort_order": "ascending"},
                       {"name": "value", "type": "int64"}]})
        write_table(
            "//tmp/in_f",
            [
                {"key": 1,"value": 2},
            ],
            sorted_by="key")

        create("table", "//tmp/out")
        
        reduce(
            in_=["<teleport=%true>//tmp/in_t1", "<foreign=%true>//tmp/in_f"],
            out="<teleport=%true>//tmp/out",
            reduce_by="key",
            join_by="key",
            command='cat; echo "key=2"',
            spec={"reducer": {"format": "dsv"}})
        assert read_table("//tmp/out") == [
            {"key": "0", "value": "0"},
            {"key": "1", "value": "1"},
            {"key": "1", "value": "2"},
            {"key": "2"},
        ]

        reduce(
            in_=["<teleport=%true>//tmp/in_t2", "<foreign=%true>//tmp/in_f"],
            out="<teleport=%true>//tmp/out",
            reduce_by="key",
            join_by="key",
            command='cat; echo "key=2"',
            spec={"reducer": {"format": "dsv"}})
        assert read_table("//tmp/out") == [{"key": 0, "value": 0}]

##################################################################

class TestSchedulerReduceCommandsSliceSize(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 3
    NUM_SCHEDULERS = 1

    DELTA_SCHEDULER_CONFIG = {
        "scheduler": {
            "watchers_update_period": 100,
            "operations_update_period": 10,
            "running_jobs_update_period": 10,
        }
    }

    DELTA_CONTROLLER_AGENT_CONFIG = {
        "controller_agent": {
            "operations_update_period": 10,
            "reduce_operation_options": {
                "min_slice_data_weight": 1,
            }
        }
    }

    @authors("gritukan")
    @pytest.mark.parametrize("tables_intersect", [False, True])
    def test_chunk_slice_size(self, tables_intersect):
        for i in range(10):
            create("table", "//tmp/in{}".format(i), attributes={
                "schema": [{"name": "key", "type": "string", "sort_order": "ascending"}],
                "chunk_writer": {"block_size": 1}, # Each block should have exactly one row to make precise slices.
                "compression_codec": "none"})
            if tables_intersect:
                write_table("//tmp/in{}".format(i), [{"key": ("%04d" % (10 * x + i))} for x in range(10)])
            else:
                # Last row ensures that chunk won't be teleported.
                write_table("//tmp/in{}".format(i), [{"key": ("%04d" % (10 * i + x))} for x in range(9)] + [{"key": ("%04d" % (9000 + i))}])
        create("table", "//tmp/out")

        op = reduce(
            in_=["//tmp/in{}".format(i) for i in range(10)],
            out="<sorted_by=[key]>//tmp/out",
            reduce_by="key",
            command="cat",
            spec={
                "job_count": 10,
                "enable_job_splitting": False,
                "reducer": {"format": "dsv"}})
        op.track()
        for chunk_id in get("//tmp/out/@chunk_ids"):
            assert 5 <= get("#" + chunk_id + "/@row_count") <= 15

##################################################################

class TestSchedulerReduceCommandsMulticell(TestSchedulerReduceCommands):
    NUM_SECONDARY_MASTER_CELLS = 2
