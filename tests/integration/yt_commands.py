import yt.yson as yson
from yt_driver_bindings import Driver, Request
from yt.common import YtError, YtResponseError, flatten, update

import __builtin__

import os, stat
import sys
import tempfile
import time
from datetime import datetime
import cStringIO
from cStringIO import StringIO


###########################################################################

driver = None
secondary_drivers = None
is_multicell = None
path_to_run_tests = None

# See transaction_client/public.h
SyncLastCommittedTimestamp   = 0x3fffffffffffff01
AsyncLastCommittedTimestamp  = 0x3fffffffffffff04
MinTimestamp                 = 0x0000000000000001

def get_driver(index=0):
    if index == 0:
        return driver
    else:
        return secondary_drivers[index - 1]

def init_driver(config, secondary_driver_configs):
    global driver
    global secondary_drivers

    driver = Driver(config=config)
    secondary_drivers = []
    for secondary_driver_config in secondary_driver_configs:
        secondary_drivers.append(Driver(config=secondary_driver_config))

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
    change(parameters, "ping_ancestor_txs", "ping_ancestor_transactions")
    return parameters

def execute_command(command_name, parameters, input_stream=None, output_stream=None, verbose=None, ignore_result=False):
    if "verbose" in parameters:
        verbose = parameters["verbose"]
        del parameters["verbose"]
    verbose = verbose is None or verbose

    if "ignore_result" in parameters:
        ignore_result = parameters["ignore_result"]
        del parameters["ignore_result"]
    ignore_result = ignore_result is None or ignore_result

    if "driver" in parameters:
        driver = parameters["driver"]
        del parameters["driver"]
    else:
        driver = get_driver()

    authenticated_user = None
    if "authenticated_user" in parameters:
        authenticated_user = parameters["authenticated_user"]
        del parameters["authenticated_user"]

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

    if verbose:
        print >>sys.stderr, str(datetime.now()), command_name, parameters
    response = driver.execute(
        Request(command_name=command_name,
                parameters=parameters,
                input_stream=input_stream,
                output_stream=output_stream,
                user=authenticated_user))

    if ignore_result:
        return

    response.wait()
    if not response.is_ok():
        error = YtResponseError(response.error())
        print >>sys.stderr, str(error)
        print >>sys.stderr
        # NB: we want to see inner errors in teamcity.
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

def multicell_sleep():
    if is_multicell:
        time.sleep(0.5)

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

def poll_job_shell(job_id, authenticated_user=None, **kwargs):
    kwargs = {"job_id": job_id, "parameters": kwargs}
    if authenticated_user:
        kwargs["authenticated_user"] = authenticated_user
    return yson.loads(execute_command('poll_job_shell', kwargs))

def abort_job(job_id, **kwargs):
    kwargs["job_id"] = job_id
    execute_command('abort_job', kwargs)

def lock(path, waitable=False, **kwargs):
    kwargs["path"] = path
    kwargs["waitable"] = waitable
    return yson.loads(execute_command('lock', kwargs))

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
    execute_command("create", kwargs)

def copy(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return yson.loads(execute_command("copy", kwargs))

def move(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return yson.loads(execute_command("move", kwargs))

def link(target_path, link_path, **kwargs):
    kwargs["target_path"] = target_path
    kwargs["link_path"] = link_path
    return yson.loads(execute_command("link", kwargs))

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
        attributes["sorted_by"] = flatten(kwargs["sorted_by"])
    kwargs["path"] = yson.to_yson_type(path, attributes=attributes)
    return execute_command("write_table", kwargs, input_stream=StringIO(value))

def select_rows(query, **kwargs):
    kwargs["query"] = query
    kwargs["verbose_logging"] = True
    return execute_command_with_output_format("select_rows", kwargs)

def _prepare_rows_stream(data, is_raw=False):
    # remove surrounding [ ]
    if not is_raw:
        data = yson.dumps(data, boolean_as_string=False, yson_type="list_fragment")
    return StringIO(data)

def insert_rows(path, data, is_raw=False, **kwargs):
    kwargs["path"] = path
    return execute_command("insert_rows", kwargs, input_stream=_prepare_rows_stream(data, is_raw))

def delete_rows(path, data, **kwargs):
    kwargs["path"] = path
    return execute_command("delete_rows", kwargs, input_stream=_prepare_rows_stream(data))

def trim_rows(path, tablet_index, trimmed_row_count, **kwargs):
    kwargs["path"] = path
    kwargs["tablet_index"] = tablet_index
    kwargs["trimmed_row_count"] = trimmed_row_count
    return execute_command_with_output_format("trim_rows", kwargs)

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

def freeze_table(path, **kwargs):
    clear_metadata_caches()
    kwargs["path"] = path
    return execute_command("freeze_table", kwargs)

def unfreeze_table(path, **kwargs):
    clear_metadata_caches()
    kwargs["path"] = path
    return execute_command("unfreeze_table", kwargs)

def reshard_table(path, arg, **kwargs):
    clear_metadata_caches()
    kwargs["path"] = path
    if isinstance(arg, int):
        kwargs["tablet_count"] = arg
    else:
        kwargs["pivot_keys"] = arg
    return execute_command("reshard_table", kwargs)

def alter_table(path, **kwargs):
    kwargs["path"] = path;
    return execute_command("alter_table", kwargs)

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

def make_batch_request(command_name, input=None, **kwargs):
    request = dict()
    request["command"] = command_name
    request["parameters"] = kwargs
    if input is not None:
        request["input"] = input
    return request

def execute_batch(requests, **kwargs):
    kwargs["requests"] = requests
    return yson.loads(execute_command("execute_batch", kwargs))

def get_batch_output(result):
    if "error" in result:
        raise YtResponseError(result["error"])
    if "output" in result:
        return result["output"]
    return None

def check_permission(user, permission, path, **kwargs):
    kwargs["user"] = user
    kwargs["permission"] = permission
    kwargs["path"] = path
    return yson.loads(execute_command("check_permission", kwargs))

class TimeoutError(Exception):
    pass

class Operation(object):
    def __init__(self):
        self.resumed_jobs = __builtin__.set()

        self._tmpdir = ""
        self._poll_frequency = 0.1

    def ensure_jobs_running(self, timeout=20.0):
        print >>sys.stderr, "Ensure operation jobs are running %s" % self.id

        jobs_path = "//sys/scheduler/orchid/scheduler/operations/{0}/running_jobs".format(self.id)
        progress_path = "//sys/scheduler/orchid/scheduler/operations/{0}/progress".format(self.id)

        # Wait till all jobs are scheduled.
        self.jobs = []
        running_count = 0
        pending_count = 0
        while (running_count == 0 or pending_count > 0) and timeout > 0:
            time.sleep(self._poll_frequency)
            timeout -= self._poll_frequency
            try:
                progress = get(progress_path + "/jobs", verbose=False)
                running_count = progress["running"]
                pending_count = progress["pending"]
            except YtResponseError as error:
                # running_jobs directory is not created yet.
                if error.is_resolve_error():
                    continue
                raise

        if timeout <= 0:
            raise TimeoutError("Jobs didn't become running within timeout")

        self.jobs = list(frozenset(ls(jobs_path)) - self.resumed_jobs)

        # Wait till all jobs are actually running.
        while not all([os.path.exists(os.path.join(self._tmpdir, "started_" + job)) for job in self.jobs]) and timeout > 0:
            time.sleep(self._poll_frequency)
            timeout -= self._poll_frequency

        if timeout <= 0:
            raise TimeoutError("Jobs didn't actually started within timeout")

    def ensure_running(self, timeout=2.0):
        print >>sys.stderr, "Ensure operation is running %s" % self.id

        state = self.get_state(verbose=False)
        while state != "running" and timeout > 0:
            time.sleep(self._poll_frequency)
            timeout -= self._poll_frequency
            state = self.get_state(verbose=False)

        if state != "running":
            raise TimeoutError("Operation didn't become running within timeout")

    def _remove_job_files(self, job):
        os.unlink(os.path.join(self._tmpdir, "started_" + job))

    def resume_job(self, job):
        print >>sys.stderr, "Resume operation job %s" % job

        self.resumed_jobs.add(job)
        self.jobs.remove(job)
        self._remove_job_files(job)

    def resume_jobs(self):
        if self.jobs is None:
            raise RuntimeError('"ensure_running" must be called before resuming jobs')

        for job in self.jobs:
            self._remove_job_files(job)

        try:
            os.rmdir(self._tmpdir)
        except OSError:
            sys.excepthook(*sys.exc_info())

    def get_job_count(self, state):
        path = "//sys/scheduler/orchid/scheduler/operations/{0}/progress/jobs/{1}".format(self.id, state)
        if not exists(path):
            return 0
        return get(path, verbose=False)

    def get_state(self, **kwargs):
        return get("//sys/operations/{0}/@state".format(self.id), **kwargs)

    def track(self):
        jobs_path = "//sys/operations/{0}/jobs".format(self.id)

        counter = 0
        while True:
            state = self.get_state(verbose=False)
            message = "Operation {0} {1}".format(self.id, state)
            if counter % 30 == 0 or state in ["failed", "aborted", "completed"]:
                print >>sys.stderr, message
            if state == "failed":
                error = get("//sys/operations/{0}/@result/error".format(self.id), verbose=False, is_raw=True)
                jobs = get(jobs_path, verbose=False)
                for job in jobs:
                    job_error_path = jobs_path + "/{0}/@error".format(job)
                    job_stderr_path = jobs_path + "/{0}/stderr".format(job)
                    if exists(job_error_path, verbose=False):
                        error = error + "\n\n" + get(job_error_path, verbose=False, is_raw=True)
                        if "stderr" in jobs[job]:
                            error = error + "\n" + read_file(job_stderr_path, verbose=False)
                raise YtError(error)
            if state == "aborted":
                raise YtError(message)
            if state == "completed":
                break
            counter += 1
            time.sleep(self._poll_frequency)

    def abort(self, **kwargs):
        kwargs["operation_id"] = self.id
        execute_command("abort_op", kwargs)

    def complete(self, **kwargs):
        kwargs["operation_id"] = self.id
        execute_command("complete_op", kwargs)

def create_tmpdir(prefix):
    basedir = os.path.join(path_to_run_tests, "tmp")
    try:
        if not os.path.exists(basedir):
            os.mkdir(basedir)
    except OSError:
        sys.excepthook(*sys.exc_info())

    tmpdir = tempfile.mkdtemp(
         prefix="{0}_{1}_".format(prefix, os.getpid()),
         dir=basedir)
    # Give full access to tmpdir, it must be accessible from user jobs
    # to implement waitable jobs.
    os.chmod(tmpdir, stat.S_IRWXU | stat.S_IRWXG | stat.S_IRWXO)
    return tmpdir


def track_path(path, timeout):
    poll_frequency = 0.1
    total_wait_time = 0
    while total_wait_time < timeout:
        if exists(path, verbose=False):
            break
        time.sleep(poll_frequency)
        total_wait_time += poll_frequency

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

    operation = Operation()

    waiting_jobs = kwargs.get("waiting_jobs", False)
    if "command" in kwargs and waiting_jobs:
        label = kwargs.get("label", "test")
        operation._tmpdir = create_tmpdir(label)
        kwargs["command"] = (
            "({1}\n"
            "touch {0}/started_$YT_JOB_ID 2>/dev/null\n"
            "{2}\n"
            "while [ -f {0}/started_$YT_JOB_ID ]; do sleep 0.1; done\n)"
            .format(operation._tmpdir, kwargs.get("precommand", ""), kwargs["command"]))

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

    operation.id = execute_command(op_type, kwargs).strip().replace('"', '')

    if waiting_jobs:
        operation.ensure_jobs_running()

    if track:
        operation.track()

    return operation

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

def create_tablet_cell(**kwargs):
    kwargs["type"] = "tablet_cell"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    return yson.loads(execute_command("create", kwargs))

def create_tablet_cell_bundle(name, **kwargs):
    kwargs["type"] = "tablet_cell_bundle"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_tablet_cell_bundle(name):
    remove("//sys/tablet_cell_bundles/" + name)
    gc_collect()

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

#########################################

def get_last_profiling_values(orchid_path, metrics):
    # To ensure that profiling updated.
    time.sleep(1)

    values = {}
    for metric in metrics:
        values[metric] = get(orchid_path + "/" + metric, verbose=False)[-1]["value"]
    return values

#########################################

def total_seconds(td):
    return float(td.microseconds + (td.seconds + td.days * 24 * 3600) * 10 ** 6) / 10 ** 6
