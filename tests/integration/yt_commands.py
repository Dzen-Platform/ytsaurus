import yt.yson as yson
from yt_driver_bindings import Driver, Request
from yt.common import YtError, flatten, update

import sys
import time
from datetime import datetime
import cStringIO
from cStringIO import StringIO


###########################################################################

driver = None

def get_driver():
    return driver

def init_driver(config):
    global driver
    driver = Driver(config=config)

def set_branch(dict, path, value):
    root = dict
    for field in path[:-1]:
        if field not in root:
            root[field] = {}
        root = root[field]
    root[path[-1]] = value

def change(dict, old, new):
    if old in dict:
        set_branch(dict, flatten(new), dict[old])
        del dict[old]

def flat(dict, key):
    if key in dict:
        dict[key] = flatten(dict[key])

def prepare_path(path):
    attributes = {}
    if isinstance(path, yson.YsonString):
        attributes = path.attributes
    result = yson.loads(execute_command("parse_ypath", parameters={"path": path}, verbose=False))
    update(result.attributes, attributes)
    return result

def prepare_paths(paths):
    return [prepare_path(path) for path in flatten(paths)]

def prepare_parameters(parameters):
    change(parameters, "tx", "transaction_id")
    change(parameters, "attr", "attributes")
    change(parameters, "ping_ancestor_txs", "ping_ancestor_transactions")
    return parameters

def execute_command(command_name, parameters, input_stream=None, output_stream=None, verbose=None):
    if "verbose" in parameters:
        verbose = parameters["verbose"]
        del parameters["verbose"]
    verbose = verbose is None or verbose

    if "driver" in parameters:
        driver = parameters["driver"]
        del parameters["driver"]
    else:
        driver = get_driver()

    user = None
    if "user" in parameters:
        user = parameters["user"]
        del parameters["user"]

    if "path" in parameters and command_name != "parse_ypath":
        parameters["path"] = prepare_path(parameters["path"])

    yson_format = yson.to_yson_type("yson", attributes={"format": "text"})
    description = driver.get_command_descriptor(command_name)
    if description.input_type() != "null" and parameters.get("input_format") is None:
        parameters["input_format"] = yson_format
    if description.output_type() != "null":
        if parameters.get("output_format") is None:
            parameters["output_format"] = yson_format
        if output_stream is None:
            output_stream = cStringIO.StringIO()

    parameters = prepare_parameters(parameters)

    yson_format = yson.to_yson_type("yson", attributes={"format": "text"})
    description = driver.get_command_descriptor(command_name)
    if description.input_type() != "null" and parameters.get("input_format") is None:
        parameters["input_format"] = yson_format
    if description.output_type() != "null":
        if parameters.get("output_format") is None:
            parameters["output_format"] = yson_format
        if output_stream is None:
            output_stream = cStringIO.StringIO()

    if verbose:
        print >>sys.stderr, str(datetime.now()), command_name, parameters
    response = driver.execute(
        Request(command_name=command_name,
                parameters=parameters,
                input_stream=input_stream,
                output_stream=output_stream,
                user=user))
    response.wait()
    if not response.is_ok():
        error = YtError(**response.error())
        print >>sys.stderr, str(error)
        print >>sys.stderr
        raise error
    if isinstance(output_stream, cStringIO.OutputType):
        result = output_stream.getvalue()
        if verbose:
            print >>sys.stderr, result
            print >>sys.stderr
        return result

def execute_command_with_output_format(command_name, kwargs, input_stream=None):
    has_output_format = "output_format" in kwargs
    if not has_output_format:
        kwargs["output_format"] = yson.loads("<format=text>yson")
    output = StringIO()
    execute_command(command_name, kwargs, input_stream=input_stream, output_stream=output)
    if not has_output_format:
        return list(yson.loads(output.getvalue(), yson_type="list_fragment"))
    else:
        return output.getvalue()

###########################################################################

def dump_job_context(job_id, path, **kwargs):
    kwargs["job_id"] = job_id
    kwargs["path"] = path
    return execute_command("dump_job_context", kwargs)

def strace_job(job_id, **kwargs):
    kwargs["job_id"] = job_id
    result = execute_command('strace_job', kwargs)
    return yson.loads(result)

def signal_job(job_id, signal_name, **kwargs):
    kwargs["job_id"] = job_id
    kwargs["signal_name"] = signal_name
    execute_command('signal_job', kwargs)

def abandon_job(job_id, **kwargs):
    kwargs["job_id"] = job_id
    execute_command('abandon_job', kwargs)

def lock(path, waitable=False, **kwargs):
    kwargs["path"] = path
    kwargs["waitable"] = waitable
    return execute_command('lock', kwargs).replace('"', '').strip('\n')

def remove(path, **kwargs):
    kwargs["path"] = path
    return execute_command('remove', kwargs)

def get(path, is_raw=False, **kwargs):
    def has_arg(name):
        return name in kwargs and kwargs[name] is not None

    kwargs["path"] = path
    result = execute_command('get', kwargs)
    return result if is_raw else yson.loads(result)

def set(path, value, is_raw=False, **kwargs):
    if not is_raw:
        value = yson.dumps(value)
    kwargs["path"] = path
    return execute_command('set', kwargs, input_stream=StringIO(value))

def create(object_type, path, **kwargs):
    kwargs["type"] = object_type
    kwargs["path"] = path
    return execute_command("create", kwargs)

def copy(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return execute_command("copy", kwargs)

def move(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return execute_command("move", kwargs)

def link(target_path, link_path, **kwargs):
    kwargs["target_path"] = target_path
    kwargs["link_path"] = link_path
    return execute_command("link", kwargs)

def exists(path, **kwargs):
    kwargs["path"] = path
    res = execute_command("exists", kwargs)
    return yson.loads(res)

def concatenate(source_paths, destination_path, **kwargs):
    kwargs["source_paths"] = source_paths
    kwargs["destination_path"] = destination_path
    return execute_command("concatenate", kwargs)

def ls(path, **kwargs):
    kwargs["path"] = path
    return yson.loads(execute_command("list", kwargs))

def read_table(path, **kwargs):
    kwargs["path"] = path
    return execute_command_with_output_format("read_table", kwargs)

def write_table(path, value, is_raw=False, **kwargs):
    if not is_raw:
        if not isinstance(value, list):
            value = [value]
        value = yson.dumps(value)
        # remove surrounding [ ]
        value = value[1:-1]

    attributes = {}
    if "sorted_by" in kwargs:
        attributes={"sorted_by": flatten(kwargs["sorted_by"])}
    kwargs["path"] = yson.to_yson_type(path, attributes=attributes)
    return execute_command("write_table", kwargs, input_stream=StringIO(value))

def select_rows(query, **kwargs):
    kwargs["query"] = query
    kwargs["verbose_logging"] = True
    return execute_command_with_output_format("select_rows", kwargs)

def _prepare_rows_stream(data):
    # remove surrounding [ ]
    return StringIO(yson.dumps(data, boolean_as_string=False)[1:-1])

def insert_rows(path, data, is_raw=False, **kwargs):
    kwargs["path"] = path
    return execute_command("insert_rows", kwargs, input_stream=_prepare_rows_stream(data))

def delete_rows(path, data, **kwargs):
    kwargs["path"] = path
    return execute_command("delete_rows", kwargs, input_stream=_prepare_rows_stream(data))

def lookup_rows(path, data, **kwargs):
    kwargs["path"] = path
    return execute_command_with_output_format("lookup_rows", kwargs, input_stream=_prepare_rows_stream(data))

def start_transaction(**kwargs):
    out = execute_command("start_tx", kwargs)
    return out.replace('"', '').strip("\n")

def commit_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return execute_command("commit_tx", kwargs)

def ping_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return execute_command("ping_tx", kwargs)

def abort_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return execute_command("abort_tx", kwargs)

def mount_table(path, **kwargs):
    clear_metadata_caches()
    kwargs["path"] = path
    return execute_command("mount_table", kwargs)

def unmount_table(path, **kwargs):
    clear_metadata_caches()
    kwargs["path"] = path
    return execute_command("unmount_table", kwargs)

def remount_table(path, **kwargs):
    kwargs["path"] = path
    return execute_command("remount_table", kwargs)

def reshard_table(path, pivot_keys, **kwargs):
    clear_metadata_caches()
    kwargs["path"] = path
    kwargs["pivot_keys"] = pivot_keys
    return execute_command("reshard_table", kwargs)

def write_file(path, data, **kwargs):
    kwargs["path"] = path
    return execute_command("write_file", kwargs, input_stream=StringIO(data))

def write_local_file(path, file_name, **kwargs):
    with open(file_name, "rt") as f:
        return write_file(path, f.read(), **kwargs)

def read_file(path, **kwargs):
    kwargs["path"] = path
    output = StringIO()
    execute_command("read_file", kwargs, output_stream=output)
    return output.getvalue();

def read_journal(path, **kwargs):
    kwargs["path"] = path
    kwargs["output_format"] = yson.loads("yson")
    output = StringIO()
    execute_command("read_journal", kwargs, output_stream=output)
    return list(yson.loads(output.getvalue(), yson_type="list_fragment"))

def write_journal(path, value, is_raw=False, **kwargs):
    if not isinstance(value, list):
        value = [value]
    value = yson.dumps(value)
    # remove surrounding [ ]
    value = value[1:-1]
    kwargs["path"] = path
    return execute_command("write_journal", kwargs, input_stream=StringIO(value))

def track_op(op_id):
    counter = 0
    while True:
        state = get("//sys/operations/%s/@state" % op_id, verbose=False)
        message = "Operation %s %s" % (op_id, state)
        if counter % 30 == 0 or state in ["failed", "aborted", "completed"]:
            print >>sys.stderr, message
        if state == "failed":
            error = get("//sys/operations/%s/@result/error" % op_id, verbose=False, is_raw=True)
            jobs = get("//sys/operations/%s/jobs" % op_id, verbose=False)
            for job in jobs:
                if exists("//sys/operations/%s/jobs/%s/@error" % (op_id, job), verbose=False):
                    error = error + "\n\n" + get("//sys/operations/%s/jobs/%s/@error" % (op_id, job), verbose=False, is_raw=True)
                    if "stderr" in jobs[job]:
                        error = error + "\n" + read_file("//sys/operations/%s/jobs/%s/stderr" % (op_id, job), verbose=False)
            raise YtError(error)
        if state == "aborted":
            raise YtError(message)
        if state == "completed":
            break
        counter += 1
        time.sleep(0.1)

def check_all_stderrs(op_id, expected_content, expected_count):
    jobs_path = "//sys/operations/{0}/jobs".format(op_id)
    assert get(jobs_path + "/@count") == expected_count
    for job_id in ls(jobs_path):
        assert read_file("{0}/{1}/stderr".format(jobs_path, job_id)) == expected_content

def start_op(op_type, **kwargs):
    op_name = None
    if op_type == "map":
        op_name = "mapper"
    if op_type == "reduce" or op_type == "join_reduce":
        op_name = "reducer"

    input_name = None
    if op_type != "erase":
        kwargs["in_"] = prepare_paths(kwargs["in_"])
        input_name = "input_table_paths"

    output_name = None
    if op_type in ["map", "reduce", "join_reduce", "map_reduce"]:
        kwargs["out"] = prepare_paths(kwargs["out"])
        output_name = "output_table_paths"
    elif "out" in kwargs:
        kwargs["out"] = prepare_path(kwargs["out"])
        output_name = "output_table_path"

    if "file" in kwargs:
        kwargs["file"] = prepare_paths(kwargs["file"])

    for opt in ["sort_by", "reduce_by", "join_by"]:
        flat(kwargs, opt)

    change(kwargs, "table_path", ["spec", "table_path"])
    change(kwargs, "in_", ["spec", input_name])
    change(kwargs, "out", ["spec", output_name])
    change(kwargs, "command", ["spec", op_name, "command"])
    change(kwargs, "file", ["spec", op_name, "file_paths"])
    change(kwargs, "sort_by", ["spec","sort_by"])
    change(kwargs, "reduce_by", ["spec","reduce_by"])
    change(kwargs, "join_by", ["spec","join_by"])
    change(kwargs, "mapper_file", ["spec", "mapper", "file_paths"])
    change(kwargs, "reduce_combiner_file", ["spec", "reduce_combiner", "file_paths"])
    change(kwargs, "reducer_file", ["spec", "reducer", "file_paths"])
    change(kwargs, "mapper_command", ["spec", "mapper", "command"])
    change(kwargs, "reduce_combiner_command", ["spec", "reduce_combiner", "command"])
    change(kwargs, "reducer_command", ["spec", "reducer", "command"])

    track = not kwargs.get("dont_track", False)
    if "dont_track" in kwargs:
        del kwargs["dont_track"]

    op_id = execute_command(op_type, kwargs).strip().replace('"', '')

    if track:
        track_op(op_id)

    return op_id

def map(**kwargs):
    change(kwargs, "ordered", ["spec", "ordered"])
    return start_op("map", **kwargs)

def merge(**kwargs):
    flat(kwargs, "merge_by")
    for opt in ["combine_chunks", "merge_by", "mode"]:
        change(kwargs, opt, ["spec", opt])
    return start_op("merge", **kwargs)

def reduce(**kwargs):
    return start_op("reduce", **kwargs)

def join_reduce(**kwargs):
    return start_op("join_reduce", **kwargs)

def map_reduce(**kwargs):
    return start_op("map_reduce", **kwargs)

def erase(path, **kwargs):
    kwargs["table_path"] = path
    change(kwargs, "combine_chunks", ["spec", "combine_chunks"])
    return start_op("erase", **kwargs)

def sort(**kwargs):
    return start_op("sort", **kwargs)

def remote_copy(**kwargs):
    return start_op("remote_copy", **kwargs)

def abort_op(op, **kwargs):
    kwargs["operation_id"] = op
    execute_command("abort_op", kwargs)

def build_snapshot(*args, **kwargs):
    get_driver().build_snapshot(*args, **kwargs)

def get_version():
    return yson.loads(execute_command("get_version", {}))

def gc_collect():
    get_driver().gc_collect()

def clear_metadata_caches():
    get_driver().clear_metadata_caches()

def create_account(name, **kwargs):
    kwargs["type"] = "account"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_account(name, **kwargs):
    remove("//sys/accounts/" + name, **kwargs)
    gc_collect()

def create_user(name, **kwargs):
    kwargs["type"] = "user"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_user(name, **kwargs):
    remove("//sys/users/" + name, **kwargs)
    gc_collect()

def create_group(name, **kwargs):
    kwargs["type"] = "group"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_group(name, **kwargs):
    remove("//sys/groups/" + name, **kwargs)
    gc_collect()

def add_member(member, group, **kwargs):
    kwargs["member"] = member
    kwargs["group"] = group
    execute_command("add_member", kwargs)

def remove_member(member, group, **kwargs):
    kwargs["member"] = member
    kwargs["group"] = group
    execute_command("remove_member", kwargs)

def create_tablet_cell(size, **kwargs):
    kwargs["type"] = "tablet_cell"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["size"] = size
    return yson.loads(execute_command("create", kwargs))

def remove_tablet_cell(id):
    remove("//sys/tablet_cells/" + id)
    gc_collect()

def create_rack(name, **kwargs):
    kwargs["type"] = "rack"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_rack(name, **kwargs):
    remove("//sys/racks/" + name, **kwargs)
    gc_collect()

#########################################
# Helpers:

def get_transactions():
    gc_collect()
    return ls("//sys/transactions")

def get_topmost_transactions():
    gc_collect()
    return ls("//sys/topmost_transactions")

def get_chunks():
    gc_collect()
    return ls("//sys/chunks")

def get_accounts():
    gc_collect()
    return ls("//sys/accounts")

def get_users():
    gc_collect()
    return ls("//sys/users")

def get_groups():
    gc_collect()
    return ls("//sys/groups")

def get_tablet_cells():
    gc_collect()
    return ls("//sys/tablet_cells")

def get_racks():
    gc_collect()
    return ls("//sys/racks")

def get_nodes():
    return ls("//sys/nodes")

