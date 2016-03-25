import pytest

from yt_env_setup import YTEnvSetup, unix_only
from yt_commands import *
from yt.yson import YsonEntity


##################################################################

class TestSchedulerReduceCommands(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 5
    NUM_SCHEDULERS = 1

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
            dont_track=True,
            in_=["//tmp/in1", "//tmp/in2"],
            out="<sorted_by=[key]>//tmp/out",
            command="cat > /dev/stderr",
            spec={
                "reducer" : {"format" : yson.loads("<format=text>yson")},
                "job_io" :
                    {"control_attributes" :
                        {"enable_table_index" : "true",
                         "enable_row_index" : "true"}},
                "job_count" : 1})

        op.track()

        job_ids = ls("//sys/operations/{0}/jobs".format(op.id))
        assert len(job_ids) == 1
        assert read_file("//sys/operations/{0}/jobs/{1}/stderr".format(op.id, job_ids[0])) == \
"""<"table_index"=1;>#;
<"row_index"=0;>#;
{"key"=1;"value"=6;};
<"table_index"=0;>#;
<"row_index"=0;>#;
{"key"=4;"value"=3;};
"""

    @unix_only
    def test_cat_teleport(self):
        create("table", "//tmp/in1")
        write_table(
            "//tmp/in1",
            [
                {"key": 0, "value": 1},
                {"key": 2, "value": 2},
                {"key": 4, "value": 3},
                {"key": 7, "value": 4}
            ],
            sorted_by = ["key", "value"])

        create("table", "//tmp/in2")
        write_table(
            "//tmp/in2",
            [
                {"key": 8, "value": 5},
                {"key": 9, "value": 6},
            ],
            sorted_by = ["key", "value"])

        create("table", "//tmp/in3")
        write_table(
            "//tmp/in3",
            [ {"key": 8, "value": 1}, ],
            sorted_by = ["key", "value"])

        create("table", "//tmp/in4")
        write_table(
            "//tmp/in4",
            [ {"key": 9, "value": 7}, ],
            sorted_by = ["key", "value"])

        create("table", "//tmp/out1")
        create("table", "//tmp/out2")

        reduce(
            in_ = ["<teleport=true>//tmp/in1", "<teleport=true>//tmp/in2", "//tmp/in3", "//tmp/in4"],
            out = ["<sorted_by=[key]; teleport=true>//tmp/out1", "<sorted_by=[key]>//tmp/out2"],
            command = "cat>/dev/fd/4",
            reduce_by = "key",
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


    def test_empty_in(self):
        create("table", "//tmp/in")

        # TODO(panin): replace it with sort of empty input (when it will be fixed)
        write_table("//tmp/in", {"foo": "bar"}, sorted_by="a")
        erase("//tmp/in")

        create("table", "//tmp/out")

        reduce(
            in_ = "//tmp/in",
            out = "//tmp/out",
            command = "cat")

        assert read_table("//tmp/out") == []

    def test_duplicate_key_columns(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")

        with pytest.raises(YtError):
            reduce(
                in_ = "//tmp/in",
                out = "//tmp/out",
                command = "cat",
                reduce_by=["a", "b", "a"])

    def test_unsorted_input(self):
        create("table", "//tmp/in")
        create("table", "//tmp/out")
        write_table("//tmp/in", {"foo": "bar"})

        with pytest.raises(YtError):
            reduce(
                in_ = "//tmp/in",
                out = "//tmp/out",
                command = "cat")

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
            command="bash reducer.sh",
            file="//tmp/reducer.sh")

        assert read_table(output_tables[0]) == [{"v": 0}]
        assert read_table(output_tables[1]) == [{"v": 1}]
        assert read_table(output_tables[2]) == [{"v": 2}]

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

        jobs_path = "//sys/operations/{0}/jobs".format(op.id)
        job_ids = ls(jobs_path)
        assert len(job_ids) == 1
        stderr_bytes = read_file("{0}/{1}/stderr".format(jobs_path, job_ids[0]))

        assert stderr_bytes.encode("hex") == \
            "010000006100000000" \
            "feffffff" \
            "010000006200000000" \
            "010000006200000000"

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

        jobs_path = "//sys/operations/{0}/jobs".format(op.id)
        job_ids = ls(jobs_path)
        assert len(job_ids) == 1
        stderr_bytes = read_file("{0}/{1}/stderr".format(jobs_path, job_ids[0]))

        assert stderr_bytes == \
"""{"key"="a";"value"="";};
<"key_switch"=%true;>#;
{"key"="b";"value"="";};
{"key"="b";"value"="";};
"""

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

        jobs_path = "//sys/operations/{0}/jobs".format(op.id)
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

    @unix_only
    def test_reduce_row_count_limit(self):
        create("table", "//tmp/input")
        for i in xrange(self.NUM_NODES):
            write_table(
                "<append=true>//tmp/input",
                [{"key": str(i), "value": "foo"}],
                sorted_by = ["key"])

        create("table", "//tmp/output")
        reduce(
            in_="//tmp/input",
            out="<row_count_limit=3>//tmp/output",
            command="((YT_JOB_INDEX >= 3)) && sleep 5; cat",
            reduce_by=["key"],
            spec={
                "reducer": {
                    "format": "dsv"
                },
                "data_size_per_job": 1,
                "max_failed_job_count": 1
            })

        assert read_table("//tmp/output") == [
            {"key":"0", "value":"foo"},
            {"key":"1", "value":"foo"},
            {"key":"2", "value":"foo"},
        ]

##################################################################

class TestSchedulerReduceCommandsMulticell(TestSchedulerReduceCommands):
    NUM_SECONDARY_MASTER_CELLS = 2
