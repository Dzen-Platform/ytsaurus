from yt.wrapper.string_iter_io import StringIterIO
import yt.yson as yson
import yt.wrapper as yt
from yt.wrapper.common import parse_bool
from yt.wrapper.format import extract_key, create_format

from cStringIO import StringIO

import pytest

def test_string_iter_io_read():
    strings = ["ab", "", "c", "de", ""]

    for add_eoln in [False, True]:
        io = StringIterIO(iter(strings), add_eoln)
        sep = "\n" if add_eoln else ""
        for c in sep.join(strings):
            assert c == io.read(1)

def test_string_iter_io_readline():
    strings = ["ab", "", "c\n", "d\ne", ""]

    for add_eoln in [False, True]:
        io = StringIterIO(iter(strings), add_eoln)
        sep = "\n" if add_eoln else ""
        lines = sep.join(strings).split("\n")
        for i in xrange(len(lines)):
            line = lines[i]
            if i + 1 != len(lines):
                line += "\n"
            assert line == io.readline()

def check_format(format, string, row):
    assert format.loads_row(string) == row
    if format.is_raw_load_supported():
        assert format.load_row(StringIO(string), raw=True) == string
    assert format.load_row(StringIO(string)) == row
    assert list(format.load_rows(StringIO(string))) == [row]

    assert format.dumps_row(row) == string
    stream = StringIO()
    format.dump_row(row, stream)
    assert stream.getvalue().rstrip("\n") == string.rstrip("\n")

def check_table_index(format, raw_row, raw_table_switcher, rows, process_output=None):
    input_stream = StringIO(''.join([raw_row, raw_table_switcher, raw_row]))
    input_stream_str = input_stream.getvalue()
    parsed_rows = list(format.load_rows(input_stream))
    assert parsed_rows == rows

    output_stream = StringIO()
    format.dump_rows(rows, output_stream)
    output_stream_str = output_stream.getvalue()
    if process_output:
        output_stream_str = process_output(output_stream_str)

    assert output_stream_str == input_stream_str

def test_yson_format():
    format = yt.YsonFormat(process_table_index=False, format="text")
    row = {"a": 1, "b": 2}
    serialized_row = '{"a"=1;"b"=2}'
    yson_rows = list(format.load_rows(StringIO(serialized_row)))
    assert yson_rows == [row]
    stream = StringIO()
    format.dump_row(row, stream)
    assert stream.getvalue().rstrip(";\n") == serialized_row

    format = yt.YsonFormat(format="binary")
    assert format.dumps_row({"a": 1}) == yson.dumps({"a": 1}, yson_format="binary") + ";"

def test_yson_table_switcher():
    format = yt.YsonFormat(format="text")
    input = '{"a"=1};\n<"table_index"=1>#;\n{"a"=1};\n'

    yson_rows = format.load_rows(StringIO(input))
    parsed_rows = [dict(yson) for yson in yson_rows]
    true_input_rows = [{'a': 1, 'input_table_index': 0, '@table_index': 0}, {'a': 1, '@table_index': 1, 'input_table_index': 1}]
    assert true_input_rows == parsed_rows
    output_rows = [{'a': 1}, {'a': 1, '@table_index': 1}]
    stream = StringIO()
    format.dump_rows(output_rows, stream)
    dumped_output = stream.getvalue()
    assert dumped_output == input

def test_dsv_format():
    format = yt.DsvFormat()
    check_format(format, "a=1\tb=2\n", {"a": "1", "b": "2"})
    check_format(format, "a=1\\t\tb=2\n", {"a": "1\t", "b": "2"})

def test_yamr_format():
    format = yt.YamrFormat(lenval=False, has_subkey=False)
    check_format(format, "a\tb\n", yt.Record("a", "b"))
    check_format(format, "a\tb\tc\n", yt.Record("a", "b\tc"))

    format = yt.YamrFormat(lenval=False, has_subkey=True)
    check_format(format, "a\tb\tc\n", yt.Record("a", "b", "c"))
    check_format(format, "a\t\tc\td\n", yt.Record("a", "", "c\td"))

    format = yt.YamrFormat(lenval=True, has_subkey=False)
    check_format(format, "\x01\x00\x00\x00a\x01\x00\x00\x00b", yt.Record("a", "b"))

def test_yamr_load_records_raw():
    records = ["a\tb\tc\n", "d\te\tf\n", "g\th\ti\n"]
    stream = StringIO(''.join(records))
    format = yt.YamrFormat(has_subkey=True)
    assert list(format.load_rows(stream, raw=True)) == records

def test_yamr_table_switcher():
    format = yt.YamrFormat(lenval=False, has_subkey=False)
    check_table_index(format, "a\tb\n", "1\n",
                      [yt.Record("a", "b", tableIndex=0), yt.Record("a", "b", tableIndex=1)])

    format = yt.YamrFormat(lenval=True, has_subkey=False)
    import struct
    table_switcher = struct.pack("ii", -1, 1)
    check_table_index(format, "\x01\x00\x00\x00a\x01\x00\x00\x00b", table_switcher,
                      [yt.Record("a", "b", tableIndex=0), yt.Record("a", "b", tableIndex=1)])

def test_json_format():
    format = yt.JsonFormat()
    check_format(format, '{"a": 1}', {"a": 1})

    stream = StringIO('{"a": 1}\n{"b": 2}')
    assert list(format.load_rows(stream)) == [{"a": 1}, {"b": 2}]

def test_json_format_table_index():
    format = yt.JsonFormat(process_table_index=True)

    stream = StringIO()
    format.dump_rows([{"a": 1, "@table_index": 1}], stream)
    assert stream.getvalue() == '{"$value": null, "$attributes": {"table_index": 1}}\n'\
                                '{"a": 1}\n'

    assert [{"@table_index": 1, "a": 1}] == \
        list(format.load_rows(StringIO('{"$value": null, "$attributes": {"table_index": 1}}\n'
                                       '{"a": 1}')))

def test_schemaful_dsv_format():
    format = yt.SchemafulDsvFormat(columns=["a", "b"])
    check_format(format, "1\t2\n", {"a": "1", "b": "2"})
    check_format(format, "\\n\t2\n", {"a": "\n", "b": "2"})

def test_yamred_dsv_format():
    format = yt.YamredDsvFormat(key_column_names=["a", "b"], subkey_column_names=["c"],
                                has_subkey=False, lenval=False)
    check_format(format, "a\tb\n", yt.Record("a", "b"))
    check_format(format, "a\tb\tc\n", yt.Record("a", "b\tc"))

def test_extract_key():
    assert extract_key(yt.Record("k", "s", "v"), ["k"]) == "k"
    assert extract_key(yt.Record("k", "v"), ["k"]) == "k"
    assert extract_key({"k": "v", "x": "y"}, ["k"]) == {"k": "v"}

def test_create_format():
    format = create_format("<has_subkey=%true>yamr")
    assert format.attributes["has_subkey"]
    assert format.name() == "yamr"
    assert format == yt.format.YamrFormat(has_subkey=True)
    assert format != yt.format.DsvFormat()

    for format_name in ["dsv", "yamred_dsv", "schemaful_dsv", "yson", "json"]:
        if format_name == "schemaful_dsv":
            format = create_format(format_name, columns=["x"])
        else:
            format = create_format(format_name)
        assert format.name() == format_name

    with pytest.raises(yt.YtError):
        create_format("best_format")

def test_raw_dump_records():
    def check_format(format, value):
        stream = StringIO()
        if isinstance(value, list):
            format.dump_rows(value, stream, raw=True)
        else:
            format.dump_row(value, stream, raw=True)
        assert stream.getvalue() == ''.join(value)

    check_format(yt.YamrFormat(), "1\t2\n")
    check_format(yt.YamredDsvFormat(), "x=1\n")
    check_format(yt.SchemafulDsvFormat(columns=["x"]), "x=1\n")
    check_format(yt.YsonFormat(), '{"x"=1; "y"=2}')
    check_format(yt.JsonFormat(), '{"x": 1}')

    check_format(yt.YamrFormat(), ["1\t2\n", "3\t4\n"])
    check_format(yt.YamredDsvFormat(), ["x=1\n", "x=2\n"])
    check_format(yt.SchemafulDsvFormat(columns=["x"]), ["x=1\n", "x=2\n"])
    check_format(yt.YsonFormat(), ['{"x"=1}', '{"x"=2}'])
    check_format(yt.JsonFormat(), ['{"x":1}', '{"x":2}'])
