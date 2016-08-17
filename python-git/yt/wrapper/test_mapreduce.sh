#!/bin/bash -eux

cd $(dirname "${BASH_SOURCE[0]}")

export YT_PREFIX="//home/wrapper_tests/"

timeout() {
    local time_to_sleep=$1 && shift
    $@ &
    pid=$!
    set +x
    for i in $(seq 1 $((10 * $time_to_sleep))); do
        if ! ps -p $pid &>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    if ps -p $pid &>/dev/null; then
        kill $pid
        return 1
    fi
    return 0
    set -x
}

prepare_table_files() {
    set +x

    echo -e "4\t5\t6\n1\t2\t3" > table_file
    TABLE_SIZE=2

    rm -f big_file
    for i in {1..10}; do
        for j in {1..10}; do
            echo -e "$i\tA\t$j" >> big_file
        done
        echo -e "$i\tX\tX" >> big_file
    done
    BIG_TABLE_SIZE=100

    set -x
}

cleanup() {
    for pid in `jobs -p`; do
        if ps ax | awk '{print $1}' | grep $pid; then
            # We use "|| true" to prevent failure in case when the process
            # terminates before we kill it.
            kill $pid || true
        fi
    done
    rm -f table_file big_file
}

die() {
    cleanup
    echo "$@" && exit 1
}

check() {
    local first="`echo -e "$1"`"
    local second="`echo -e "$2"`"
    [ "${first}" = "${second}" ] || die "Test fail $1 does not equal $2"
}

check_failed() {
    set +e
    eval $1
    if [ "$?" = "0" ]; then
        die "Command \"$@\" should fail"
    fi
    set -e
}

test_base_functionality()
{
    ./mapreduce -list
    ./mapreduce -list -prefix //unexisting/path
    ./mapreduce -drop "ignat/temp"
    ./mapreduce -write "ignat/temp" <table_file
    ./mapreduce -move -src "ignat/temp" -dst "ignat/other_table"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/other_table"`"
    ./mapreduce -copy -src "ignat/other_table" -dst "ignat/temp"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/other_table"`"
    ./mapreduce -copy -src "ignat/other_table" -dst "ignat/other_table"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/other_table"`"
    ./mapreduce -drop "ignat/temp"
    ./mapreduce -sort  -src "ignat/other_table" -dst "ignat/other_table"
    check "1\t2\t3\n4\t5\t6\n" "`./mapreduce -read "ignat/other_table"`"
    ./mapreduce -sort "ignat/other_table"
    check "1\t2\t3\n4\t5\t6\n" "`./mapreduce -read "ignat/other_table"`"
    check "4\t5\t6\n" "`./mapreduce -read "ignat/other_table" -lowerkey 3`"
    ./mapreduce -map "cat" -src "ignat/other_table" -dst "ignat/mapped" -ytspec '{"job_count": 10}'
    check 2 `./mapreduce -read "ignat/mapped" | wc -l`
    ./mapreduce -map "cat" -src "ignat/other_table" -src "ignat/mapped" \
        -dstappend "ignat/temp"
    ./mapreduce -orderedmap "cat" -src "ignat/other_table" -src "ignat/mapped" \
        -dst "ignat/temp" -append
    check 8 `./mapreduce -read "ignat/temp" | wc -l`

    ./mapreduce -reduce "cat" -src "ignat/other_table" -dst "ignat/temp"
    check 2 `./mapreduce -read "ignat/temp" | wc -l`

    MR_TABLE_PREFIX="ignat/" ./mapreduce -reduce "cat" -src "other_table" -dst "temp2"
    check 2 `./mapreduce -read "ignat/temp2" | wc -l`

    ./mapreduce -hash-reduce "cat" -src "ignat/other_table" -dst "ignat/temp"
    check 2 `./mapreduce -read "ignat/temp" | wc -l`
}

test_copy_move()
{

    ./mapreduce -write "ignat/table" <table_file
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/table"`"
    ./mapreduce -copy -src "ignat/unexisting_table" -dstappend "ignat/table"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/table"`"
    ./mapreduce -move -src "ignat/unexisting_table" -dstappend "ignat/table"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/table"`"
    ./mapreduce -copy -src "ignat/unexisting_table" -dst "ignat/table"
    check "" "`./mapreduce -read "ignat/table"`"
}

test_list()
{
    ./mapreduce -write "ignat/test_dir/table1" <table_file
    ./mapreduce -write "ignat/test_dir/table2" <table_file
    ./yt create table "ignat/test_dir/table3"

    export YT_IGNORE_EMPTY_TABLES_IN_MAPREDUCE_LIST=1
    export YT_USE_YAMR_STYLE_PREFIX=1

    check "ignat/test_dir/table1\nignat/test_dir/table2\n" "`./mapreduce -list -prefix "ignat/test_dir"`"
    check "ignat/test_dir/table1\nignat/test_dir/table2\n" "`./mapreduce -list -prefix "ignat/test_dir/"`"
    check "ignat/test_dir/table1\nignat/test_dir/table2\n" "`./mapreduce -list -prefix "ignat/test_dir/tab"`"
    check "ignat/test_dir/table1\n" "`./mapreduce -list -prefix "ignat/test_dir/table1"`"
    check "ignat/test_dir/table1\n" "`./mapreduce -list -exact "ignat/test_dir/table1"`"
    check "test_dir/table1\n" "`MR_TABLE_PREFIX='ignat/' ./mapreduce -list -exact "test_dir/table1"`"
    check "" "`./mapreduce -list -exact "ignat/test_dir/table"`"
    check "" "`./mapreduce -list -exact "ignat/test_dir"`"
    check_failed './mapreduce -list -exact "ignat/test_dir/" -prefix "ignat"'

    check "ignat/test_dir/table1\n" "`./mapreduce -list -prefix "ignat/test_dir/table" -jsonoutput | python2 -c "import json, sys; print json.load(sys.stdin)[0]['name']"`"
    check "ignat/test_dir/table2\n" "`./mapreduce -list -prefix "ignat/test_dir/table" -jsonoutput | python2 -c "import json, sys; print json.load(sys.stdin)[1]['name']"`"
    check "[]\n" "`./mapreduce -list -exact "ignat/test_dir/table" -jsonoutput`"

    unset YT_IGNORE_EMPTY_TABLES_IN_MAPREDUCE_LIST

    check "ignat/test_dir/table1\nignat/test_dir/table2\nignat/test_dir/table3\n" "`./mapreduce -list -prefix "ignat/test_dir"`"

    unset YT_USE_YAMR_STYLE_PREFIX

    check "table1\ntable2\ntable3\n" "`./mapreduce -list -prefix "${YT_PREFIX}ignat/test_dir/"`"
}

test_codec()
{
    ./mapreduce -write "ignat/temp" <table_file
    check_failed './mapreduce -write "ignat/temp" -codec "none" <table_file'

    # We cannot write to existing table with replication factor
    ./mapreduce -drop "ignat/temp"
    ./mapreduce -write "ignat/temp" -codec "zlib9" -replicationfactor 5 <table_file
    check 5 "`./mapreduce -get "ignat/temp/@replication_factor"`"
}

test_many_output_tables()
{
    # Test many output tables
    echo -e "#!/usr/bin/env python
import os
import sys

if __name__ == '__main__':
    for line in sys.stdin:
        pass

    for descr in range(3, 6):
        os.write(descr, '{0}\\\t{0}\\\t{0}\\\n'.format(descr))
    " >many_output_mapreduce.py
    chmod +x many_output_mapreduce.py

    ./mapreduce -map "./many_output_mapreduce.py" -src "ignat/temp" -dst "ignat/out1" -dst "ignat/out2" -dst "ignat/out3" -file "many_output_mapreduce.py"
    for (( i=1 ; i <= 3 ; i++ )); do
        check 1 "`./mapreduce -read "ignat/out$i" | wc -l`"
    done

    rm -f "many_output_mapreduce.py"
}

test_chunksize()
{
    ./mapreduce -write "ignat/temp" -chunksize 1 <table_file
    ./mapreduce -get "ignat/temp/@"
    check 2 "`./mapreduce -get "ignat/temp/@chunk_count"`"
}

test_mapreduce()
{
    ./mapreduce -subkey -write "ignat/temp" <big_file
    ./mapreduce -subkey -src "ignat/temp" -dst "ignat/reduced" \
        -map 'grep "A"' \
        -reduce 'awk '"'"'{a[$1]+=$3} END {for (i in a) {print i"\t\t"a[i]}}'"'"
    check 10 "`./mapreduce -subkey -read "ignat/reduced" | wc -l`"
}

test_input_output_format()
{
    ./mapreduce -subkey -write "ignat/temp" <table_file

    echo -e "#!/usr/bin/env python
import sys

if __name__ == '__main__':
    for line in sys.stdin:
        pass

    for i in range(2):
        sys.stdout.write('k={0}\\\ts={0}\\\tv={0}\\\n'.format(i))
    " >reformat.py

    ./mapreduce -subkey -outputformat "dsv" -map "python reformat.py" -file "reformat.py" -src "ignat/temp" -dst "ignat/reformatted"
    for i in {0..1}; do
        for f in k s v; do
            ./mapreduce -dsv -read "ignat/reformatted" | grep "$f=$i"
        done
    done

    rm reformat.py

    echo "{k=1;v=2}" | ./mapreduce -format yson -write "ignat/table"
    # TODO: We need to check equality in order independent manner
    check "k=1\tv=2" "`./mapreduce -format dsv -read "ignat/table"`"
}

test_transactions()
{
    ./mapreduce -subkey -write "ignat/temp" <table_file
    TX=`./mapreduce -starttx`
    ./mapreduce -renewtx "$TX"
    ./mapreduce -subkey -write "ignat/temp" -append -tx "$TX" < table_file
    ./mapreduce -set "ignat/temp/@my_attr"  -value 10 -tx "$TX"

    check_failed './mapreduce -get "ignat/temp/@my_attr"'
    check 2 "`./mapreduce -read "ignat/temp" | wc -l`"

    ./mapreduce -committx "$TX"

    check 10 "`./mapreduce -get "ignat/temp/@my_attr"`"
    check 4 "`./mapreduce -read "ignat/temp" | wc -l`"
}

test_range_map()
{
    ./mapreduce -subkey -write "ignat/temp" <table_file
    ./mapreduce -map 'awk '"'"'{sum+=$1+$2} END {print "\t"sum}'"'" -src "ignat/temp{key,value}" -dst "ignat/sum"
    check "\t14" "`./mapreduce -read "ignat/sum"`"
}

test_uploaded_files()
{
    ./mapreduce -subkey -write "ignat/temp" <table_file

    echo -e "#!/usr/bin/env python
import sys

if __name__ == '__main__':
    for line in sys.stdin:
        pass

    for i in range(5):
        sys.stdout.write('{0}\\\t{1}\\\t{2}\\\n'.format(i, i * i, i * i * i))
    " >my_mapper.py
    chmod +x my_mapper.py

    ./mapreduce -drop ignat/dir/mapper.py
    ./mapreduce -listfiles >&2
    initial_number_of_files="`./mapreduce -listfiles | wc -l`"

    ./mapreduce -upload ignat/dir/mapper.py -executable < my_mapper.py
    cat my_mapper.py | ./mapreduce -upload ignat/dir/mapper.py -executable
    ./mapreduce -download ignat/dir/mapper.py > my_mapper_copy.py
    diff my_mapper.py my_mapper_copy.py

    check $((1 + ${initial_number_of_files})) "`./mapreduce -listfiles | wc -l`"

    ./mapreduce -subkey -map "./mapper.py" -ytfile "ignat/dir/mapper.py" -src "ignat/temp" -dst "ignat/mapped"
    check 5 "`./mapreduce -subkey -read "ignat/mapped" | wc -l`"

    rm -f my_mapper.py my_mapper_copy.py
}

test_ignore_positional_arguments()
{
    ./mapreduce -list "" "123" >/dev/null
}

test_stderr()
{
    ./mapreduce -subkey -write "ignat/temp" <table_file
    check_failed "./mapreduce -subkey -map 'cat &>2 && exit(1)' -src 'ignat/temp' -dst 'ignat/tmp' 2>/dev/null"
}

test_spec()
{
    ./mapreduce -subkey -write "ignat/input" <table_file
    ./mapreduce -map 'cat >/dev/null; echo -e "${YT_OPERATION_ID}\t"' \
        -ytspec '{"opt1": 10, "opt2": {"$attributes": {"my_attr": "ignat"}, "$value": 0.5}}' \
        -src "ignat/input" \
        -dst "ignat/output"

    op_id="`./mapreduce -read "ignat/output" | tr -d '[[:space:]]'`"
    check "10" "`./mapreduce -get "//sys/operations/${op_id}/@spec/opt1"`"
    check "0.5" "`./mapreduce -get "//sys/operations/${op_id}/@spec/opt2" | python2 -c 'import sys, json; print json.loads(sys.stdin.read())["$value"]'`"

    YT_SPEC='{"opt3": "hello", "opt4": {"$attributes": {}, "$value": null}}' ./mapreduce \
        -map 'cat >/dev/null; echo -e "${YT_OPERATION_ID}\t"' \
        -src "ignat/input" \
        -dst "ignat/output"

    op_id="`./mapreduce -read "ignat/output" | tr -d '[[:space:]]'`"
    check '"hello"' "`./mapreduce -get "//sys/operations/${op_id}/@spec/opt3"`"
    check 'null' "`./mapreduce -get "//sys/operations/${op_id}/@spec/opt4"`"
}

test_smart_format()
{
    format='{
    "$value":"yamred_dsv",
    "$attributes":{
        "key_column_names":["x","y"],
        "subkey_column_names":["subkey"],
        "has_subkey":"true"
    }
}'
    same_format_with_other_repr='{
    "$value":"yamred_dsv",
    "$attributes":{
        "key_column_names":["x","y"],
        "subkey_column_names":["subkey"],
        "has_subkey":"true"
    }
}'
    export YT_SMART_FORMAT=1
    ./mapreduce -createtable "ignat/smart_x"
    ./mapreduce -createtable "ignat/smart_z"
    ./mapreduce -createtable "ignat/smart_w"

    ./mapreduce -set "ignat/smart_x/@_format" -value "$format"
    ./mapreduce -set "ignat/smart_z/@_format" -value "$format"
    ./mapreduce -set "ignat/smart_w/@_format" -value "$same_format_with_other_repr"

    # test read/write
    echo -e "1 2\t\tz=10" | ./mapreduce -subkey -write "ignat/smart_x"
    echo -e "3 4\t\tz=20" | ./mapreduce -subkey -write "ignat/smart_w"
    check "1 2\tz=10" "`./mapreduce -read "ignat/smart_x"`"
    check "`printf "\x00\x00\x00\x031 2\x00\x00\x00\x04z=10"`" "`./mapreduce -lenval -read "ignat/smart_x"`"
    check "1 2\t\tz=10" "`./mapreduce -subkey -read "ignat/smart_x"`"
    # test columns
    ranged_table='ignat/smart_x{x,z}'
    # disable brace expansion
    set +B
    check_failed "./mapreduce -read ${ranged_table}"
    set -B
    check "x=1\tz=10" "`./mapreduce -read ${ranged_table} -dsv`"
    ./mapreduce -map cat -src ignat/smart_x -src ignat/smart_w -dst ignat/output
    ./mapreduce -read ignat/output -dsv
    ./mapreduce -sort -src ignat/output -dst ignat/output
    check "key=1 2\nkey=3 4" "`./mapreduce -read ignat/output\{key\} -dsv`"

    unset YT_SMART_FORMAT
    # write in yamr
    echo -e "1 2\t\tz=10" | ./mapreduce -subkey -write "ignat/smart_y"
    # convert to yamred_dsv
    ./mapreduce -smartformat -map "cat" -src "ignat/smart_y" -src "fake" -dst "ignat/smart_x"
    check "1 2\tz=10" "`./mapreduce -smartformat -read "ignat/smart_x"`"

    check_failed './mapreduce -smartformat -map "cat" -src "ignat/smart_y" -src "fake" -dst "ignat/smart_x" -dst "ignat/some_table"'
    ./mapreduce -smartformat -map "cat" -src "ignat/smart_y" -src "fake" -dst "ignat/smart_x" -dst "ignat/smart_z" -outputformat "yamr"
    check "1 2\tz=10" "`./mapreduce -read "ignat/smart_x"`"
    check_failed './mapreduce -smartformat -read "ignat/smart_x"'

    export YT_SMART_FORMAT=1
    echo -e "1 2\t\tz=10" | ./mapreduce -subkey -write "ignat/smart_x"
    ./mapreduce -copy -src "ignat/smart_x" -dst "ignat/smart_z"

    ./mapreduce -map "cat" -src "ignat/smart_x" -src "ignat/smart_z" -dst "ignat/smart_z"
    check "1 2\tz=10\n1 2\tz=10" "`./mapreduce -read "ignat/smart_z"`"

    ./mapreduce -map "cat" -reduce "cat" -src "ignat/smart_x" -dst "ignat/smart_y"
    check "1 2\tz=10" "`./mapreduce -read "ignat/smart_y"`"

    echo -e "1 1\t" | ./mapreduce -write "ignat/smart_x" -append
    ./mapreduce -sort -src "ignat/smart_x" -dst "ignat/smart_x"
    check "1 1\t\n1 2\tz=10" "`./mapreduce -read "ignat/smart_x"`"

    # TODO(ignat): improve this test to check that reduce is made by proper columns
    echo -e "1 2\t\tz=1" | ./mapreduce -write "ignat/smart_x" -append

    ./mapreduce -read "ignat/smart_x" -dsv
    ./mapreduce -read "ignat/smart_x"

    ./mapreduce -reduce "tr '=' ' ' | awk '{sum+=\$4} END {print sum \"\t\"}'" -src "ignat/smart_x" -dst "ignat/output" -jobcount 2
    check "11\t" "`./mapreduce -read "ignat/output"`"
}

test_drop()
{
    ./mapreduce -subkey -write "ignat/xxx/yyy/zzz" <table_file
    ./mapreduce -drop "ignat/xxx/yyy/zzz"
    check_failed './mapreduce -get "ignat/xxx"'
}

test_create_table()
{
    ./mapreduce -createtable "ignat/empty_table"
    ./mapreduce -set "ignat/empty_table/@xxx" -value '"my_value"'
    echo -e "x\ty" | ./mapreduce -write "ignat/empty_table" -append
    check '"my_value"' "`./mapreduce -get "ignat/empty_table/@xxx"`"
}

test_sortby_reduceby()
{
    # It calculates sum of c2 grouped by c2
    echo -e "#!/usr/bin/env python
from __future__ import print_function
import sys
from itertools import groupby, starmap

def parse(line):
    d = dict(x.split('=') for x in line.split())
    return (d['c3'], d['c2'])

def aggregate(key, recs):
    recs = list(recs)
    for i in xrange(len(recs) - 1):
        assert recs[i][1] <= recs[i + 1][1]
    return key, sum(map(lambda x: int(x[1]), recs))

if __name__ == '__main__':
    recs = map(parse, sys.stdin.readlines())
    for key, num in starmap(aggregate, groupby(recs, lambda rec: rec[0])):
        print('c3=%s	c2=%d' % (key, num))
    " >my_reducer.py
    chmod +x my_reducer.py

    echo -e "#!/usr/bin/env python
from __future__ import print_function
import sys

if __name__ == '__main__':
    for line in sys.stdin:
        print('\t'.join(k + '=' + v for k, v in sorted(x.split('=') for x in line.split())))
    " >order.py
    chmod +x order.py

    echo -e "c1=1\tc2=2\tc3=z\n"\
            "c1=1\tc2=3\tc3=x\n"\
            "c1=2\tc2=2\tc3=x" | ./mapreduce -dsv -write "ignat/test_table"
    ./mapreduce -sort -src "ignat/test_table" -dst "ignat/sorted_table" -sortby "c3" -sortby "c2"
    ./mapreduce -reduce "./my_reducer.py" -src "ignat/sorted_table{c3,c2}" -dst "ignat/reduced_table" -reduceby "c3" -file "my_reducer.py" -dsv

    check "`echo -e "c3=x\tc2=5\nc3=z\tc2=2\n" | ./order.py`" "`./mapreduce -read "ignat/reduced_table" -dsv | ./order.py`"

    rm -f my_reducer.py order.py

    echo -e "a=1\tb=2\na=1\tb=1" | ./mapreduce -dsv -write "<sorted_by=[a]>ignat/test_table"

    ./mapreduce -reduce "cat" -src "ignat/test_table" -dst "ignat/reduced_table" -reduceby "a" -dsv
    check "`echo -e "a=1\tb=2\na=1\tb=1"`" "`./mapreduce -read "ignat/reduced_table" -dsv`"

    ./mapreduce -reduce "cat" -src "ignat/test_table" -dst "ignat/reduced_table" -reduceby "a" -sortby "a" -sortby "b" -dsv
    check "`echo -e "a=1\tb=1\na=1\tb=2"`" "`./mapreduce -read "ignat/reduced_table" -dsv`"

    ./mapreduce -reduce "cat" -src "ignat/test_table" -dst "ignat/reduced_table" -reduceby "b" -sortby "b" -dsv
    check "`echo -e "b=1\ta=1\nb=2\ta=1"`" "`./mapreduce -read "ignat/reduced_table" -dsv`"
}

test_empty_destination()
{
    ./mapreduce -write "ignat/empty_table" </dev/null
    check_failed './mapreduce -map "cat" -src "ignat/empty_table"'
}

test_dsv_reduce()
{
    echo -e "x=10\nx=0" | ./mapreduce -dsv -write "ignat/empty_table"
    ./mapreduce -dsv -reduce "cat" -reduceby "x" -src "ignat/empty_table" -dst "ignat/empty_table"
}

test_slow_write()
{
    gen_data()
    {
        sleep $1
        echo -e "a\tb"
    }
    ./mapreduce -drop "ignat/some_table"
    gen_data 1 | ./mapreduce -write "ignat/some_table" -timeout 2000
    check "`echo -e "a\tb"`" "`./mapreduce -read "ignat/some_table"`"
    gen_data 5 | ./mapreduce -write "ignat/some_table" -timeout 1000
    check "`echo -e "a\tb"`" "`./mapreduce -read "ignat/some_table"`"
}

test_many_dst_write()
{
    echo -e "a\tb\n1\nc\td\n0\ne\tf" | ./mapreduce -write -dst "ignat/A" -dst "ignat/B"
    check "`echo -e "a\tb\ne\tf"`" "`./mapreduce -read "ignat/A"`"
    check "`echo -e "c\td"`" "`./mapreduce -read "ignat/B"`"
}

test_dstsorted()
{
    echo -e "x\t10\ny\t15" | ./mapreduce -writesorted ignat/some_table
    check "$TRUE" "`./mapreduce -get ignat/some_table/@sorted`"

    ./mapreduce -reduce "grep x" -src ignat/some_table -dstsorted ignat/some_table
    check "$TRUE" "`./mapreduce -get ignat/some_table/@sorted`"

    echo -e "x\t10\ny\t15" | ./mapreduce -write ignat/some_table
    ./mapreduce -reduce "grep x" -src ignat/some_table -dstsorted ignat/some_table
    check "$TRUE" "`./mapreduce -get ignat/some_table/@sorted`"

    check_failed "./mapreduce -map cat -src ignat/some_table -dstsorted ignat/some_table -format dsv"
}

test_custom_fs_rs()
{
    echo -e "x y z" | ./mapreduce -fs " " -write ignat/some_table
    check "`echo -e "x\ty z"`" "`./mapreduce -read ignat/some_table`"
}

test_write_with_tx()
{
    gen_data()
    {
        sleep $1
        echo -e "a\tb"
    }
    ./mapreduce -drop "ignat/some_table"

    TX=`./mapreduce -starttx`
    gen_data 2 | ./mapreduce -write "ignat/some_table" -tx $TX &

    sleep 1
    check "0" "`./mapreduce -read "ignat/some_table" | wc -l`"

    sleep 2
    check "0" "`./mapreduce -read "ignat/some_table" | wc -l`"

    wait

    ./mapreduce -committx $TX
    check "1" "`./mapreduce -read "ignat/some_table" | wc -l`"
}

test_table_file()
{
    echo "x=0" | ./mapreduce -dsv -write "ignat/input"
    echo "field=10" | ./mapreduce -dsv -write "ignat/dictionary"
    ./mapreduce -map "cat >/dev/null; cat dictionary" -dsv -src "ignat/input" -dst "ignat/output" -ytfile "<format=dsv>ignat/dictionary"
    check "field=10" "`./mapreduce -dsv -read "ignat/output"`"
}

test_unexisting_input_tables()
{
    echo "x=0" | ./mapreduce -dsv -write "ignat/output"
    ./mapreduce -map "cat" -src "ignat/unexisting1" -src "ignat/unexisting2" -dst "ignat/output"
    check "" "`./mapreduce -read ignat/output`"
}

test_copy_files()
{
    echo -e "MY CONTENT" >test_file
    cat test_file | ./mapreduce -upload "ignat/my_file"
    ./mapreduce -copy -src "ignat/my_file" -dst "ignat/other_file"
    check "MY CONTENT" "`./mapreduce -download ignat/other_file`"
    rm test_file
}

test_write_lenval()
{
    echo -n -e "\\x01\\x00\\x00\\x00a\\x01\\x00\\x00\\x00b" | ./mapreduce -lenval -write "ignat/lenval_table"
    check "a\tb" "`./mapreduce -read "ignat/lenval_table"`"
}

test_force_drop()
{
    gen_data()
    {
        stdbuf -o 0 echo -e "a\tb"
        sleep 4
        echo -e "x\ty"
    }

    ./mapreduce -drop "ignat/some_table" -force
    ./mapreduce -createtable "ignat/some_table"

    gen_data | ./mapreduce -append -write "ignat/some_table" &
    bg_pid=$!

    sleep 2

    ./mapreduce -drop "ignat/some_table" -force

    check "" "`./mapreduce -read ignat/some_table`"

    kill $bg_pid || true
}

test_parallel_dstappend()
{
    echo -e "x\t10" | ./mapreduce -write ignat/table

    run_op()
    {
        timeout 14 ./mapreduce -map "cat" -src ignat/table -dstappend ignat/output_table
        if [ "$?" = 0 ]; then
            echo "xxx" >> sync_file
        fi
    }

    touch sync_file
    ./mapreduce -createtable ignat/output_table
    run_op &
    run_op &

    local ok=0
    for i in {1..15}; do
        lines=`cat sync_file | wc -l`
        if [ "$lines" = "2" ]; then
            rm -f sync_file
            check "2" "`./mapreduce -read "ignat/output_table" | wc -l`"
            ok=1
            break
        fi
        sleep 1
    done

    if [ "$ok" = 0 ]; then
        rm -f sync_file
        die "Two simple operations doesn't finish correctly in 30 seconds"
    fi
}

test_many_to_many_copy_move()
{
    ./mapreduce -write "ignat/in1" <table_file
    ./mapreduce -write "ignat/in2" <table_file
    ./mapreduce -move -src "ignat/in1" -dst "ignat/out1" -src "ignat/in2" -dst "ignat/out2"

    check "" "`./mapreduce -read "ignat/in1"`"
    check "" "`./mapreduce -read "ignat/in2"`"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/out1"`"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/out2"`"

    ./mapreduce -copy -src "ignat/out1" -dst "ignat/in1" -src "ignat/out2" -dst "ignat/in2"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/in1"`"
    check "4\t5\t6\n1\t2\t3\n" "`./mapreduce -read "ignat/in2"`"
}

test_missing_prefix()
{
    local prefix="$YT_PREFIX"
    unset YT_PREFIX

    ./mapreduce -get "tmp/@key"

    export YT_PREFIX="$prefix"
}

test_table_record_index()
{
    echo -e "a\t1" | ./mapreduce -writesorted ignat/tableA
    echo -e "b\t2" | ./mapreduce -writesorted ignat/tableB

    tempfile=$(mktemp /tmp/test_mapreduce_binary.XXXXXX)
    chmod 777 "$tempfile"

    ./mapreduce -reduce "cat > $tempfile" \
                -src ignat/tableA \
                -src ignat/tableB \
                -dst ignat/dst \
                -tablerecordindex

    check "0\n0\na\t1\n1\n0\nb\t2" "$(cat $tempfile)"
    rm -rf $tempfile
}

test_opts()
{
    ./mapreduce -write "tmp/input" <table_file
    ./mapreduce -map "cat" -src "tmp/input" -dst "tmp/output"
    check "1" "$(./mapreduce -get tmp/output/@chunk_count)"
    ./mapreduce -map "cat" -src "tmp/input" -dst "tmp/output" -opt "jobcount=2"
    check "2" "$(./mapreduce -get tmp/output/@chunk_count)"
    ./mapreduce -map "cat" -src "tmp/input" -dst "tmp/output" -opt "cpu.intensive.mode=1"
    check "2" "$(./mapreduce -get tmp/output/@chunk_count)"
    MR_OPT="cpu.intensive.mode=1" ./mapreduce -map "cat" -src "tmp/input" -dst "tmp/output"
    check "2" "$(./mapreduce -get tmp/output/@chunk_count)"
    MR_OPT="cpu.intensive.mode=1" ./mapreduce -map "cat" -src "tmp/input" -dst "tmp/output" -opt "cpu.intensive.mode=0"
    check "1" "$(./mapreduce -get tmp/output/@chunk_count)"
}

test_defrag()
{
    echo -e "a\t1" | ./mapreduce -writesorted ignat/input
    echo -e "b\t2" | ./mapreduce -append -writesorted ignat/input

    for defrag in "" "full"; do
        ./mapreduce -defrag $defrag -src ignat/input -dst ignat/output
        check "`echo -e "a\t1\nb\t2"`" "`./mapreduce -read ignat/output`"
        check "$TRUE" "`./mapreduce -get ignat/output/@sorted`"
        check "1" "`./mapreduce -get ignat/output/@chunk_count`"
    done
}

test_archive_and_transform()
{
    tempfile=$(mktemp /tmp/test_mapreduce_binary_config.XXXXXX)
    echo "{transform_options={desired_chunk_size=10000000}}" >$tempfile
    export YT_CONFIG_PATH="$tempfile"

    echo -e "a\t1\nb\t2" | ./mapreduce -write ignat/input

    ./mapreduce -archive ignat/input -erasurecodec none
    check '"zlib_9"' "`./mapreduce -get ignat/input/@compression_codec`"
    check '"none"' "`./mapreduce -get ignat/input/@erasure_codec`"

    ./mapreduce -unarchive ignat/input
    # Check nothing

    ./mapreduce -transform -src ignat/input -dst ignat/output -codec "zlib_6"
    check '"zlib_6"' "`./mapreduce -get ignat/output/@compression_codec`"
    check '"none"' "`./mapreduce -get ignat/output/@erasure_codec`"

    rm -rf "$tempfile"
    unset YT_CONFIG_PATH
}

prepare_table_files
test_base_functionality
test_copy_move
test_list
test_codec
test_many_output_tables
test_sortby_reduceby
test_chunksize
test_mapreduce
test_input_output_format
test_transactions
test_range_map
test_uploaded_files
test_ignore_positional_arguments
test_stderr
test_spec
test_smart_format
test_drop
test_create_table
test_empty_destination
test_dsv_reduce
test_slow_write
test_many_dst_write
test_dstsorted
test_custom_fs_rs
test_write_with_tx
test_table_file
test_unexisting_input_tables
test_copy_files
test_write_lenval
test_force_drop
test_parallel_dstappend
test_many_to_many_copy_move
test_missing_prefix
test_table_record_index
test_opts
test_defrag
test_archive_and_transform

cleanup
