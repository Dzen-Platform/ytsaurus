import yt.yson as yson
from yt_driver_bindings import Driver, Request
import yt_driver_bindings
from yt.common import YtError, YtResponseError, flatten, update

import __builtin__

import os, stat
import sys
import tempfile
import time
from datetime import datetime, timedelta
import cStringIO
from cStringIO import StringIO


###########################################################################

clusters_drivers = {}
is_multicell = None
path_to_run_tests = None

# See transaction_client/public.h
SyncLastCommittedTimestamp   = 0x3fffffffffffff01
AsyncLastCommittedTimestamp  = 0x3fffffffffffff04
MinTimestamp                 = 0x0000000000000001

def is_debug():
    from build_type import BUILD_TYPE
    return BUILD_TYPE == "Debug"

def get_driver(cell_index=0, cluster="primary"):
    if cluster not in clusters_drivers:
        return None

    return clusters_drivers[cluster][cell_index]

def _get_driver(driver):
    if driver is None:
        return get_driver()
    else:
        return driver

def init_drivers(clusters):
    for instance in clusters:
        if instance.master_count > 0:
            secondary_driver_configs = [instance.configs["driver_secondary_" + str(i)]
                                        for i in xrange(instance.secondary_master_cell_count)]
            driver = Driver(config=instance.configs["driver"])
            secondary_drivers = []
            for secondary_driver_config in secondary_driver_configs:
                secondary_drivers.append(Driver(config=secondary_driver_config))

            clusters_drivers[instance._cluster_name] = [driver] + secondary_drivers

            if instance._cluster_name == "primary":
                yt_driver_bindings.configure_logging(instance.driver_logging_config)

def terminate_drivers():
    for drivers in clusters_drivers.itervalues():
        __builtin__.map(lambda d: d.terminate(), drivers)

    clusters_drivers.clear()

def set_branch(dict, path, value):
    root = dict
    for field in path[:-1]:
        assert isinstance(field, str), "non-string keys are not allowed in command parameters"
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

def execute_command(command_name, parameters, input_stream=None, output_stream=None, verbose=None, verbose_error=None, ignore_result=False):
    if "verbose" in parameters:
        verbose = parameters["verbose"]
        del parameters["verbose"]
    verbose = verbose is None or verbose

    if "verbose_error" in parameters:
        verbose = parameters["verbose_error"]
        del parameters["verbose_error"]
    verbose_error = verbose_error is None or verbose_error

    if "ignore_result" in parameters:
        ignore_result = parameters["ignore_result"]
        del parameters["ignore_result"]
    ignore_result = ignore_result is None or ignore_result

    driver = _get_driver(parameters.pop("driver", None))

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
        if verbose_error:
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

def get_job_input(operation_id, job_id, **kwargs):
    kwargs["operation_id"] = operation_id
    kwargs["job_id"] = job_id
    return execute_command("get_job_input", kwargs)

def get_job_stderr(operation_id, job_id, **kwargs):
    kwargs["operation_id"] = operation_id
    kwargs["job_id"] = job_id
    return execute_command("get_job_stderr", kwargs)

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

def interrupt_job(job_id, interrupt_timeout=10000, **kwargs):
    kwargs["job_id"] = job_id
    kwargs["interrupt_timeout"] = interrupt_timeout
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
    return yson.loads(execute_command("create", kwargs))

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
    if not is_raw:
        return execute_command("insert_rows", kwargs, input_stream=_prepare_rows_stream(data))
    else:
        return execute_command("insert_rows", kwargs, input_stream=StringIO(data))

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
    return yson.loads(execute_command("start_tx", kwargs))

def commit_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return execute_command("commit_tx", kwargs)

def ping_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return execute_command("ping_tx", kwargs)

def abort_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    return execute_command("abort_tx", kwargs)

def generate_timestamp(**kwargs):
    return yson.loads(execute_command("generate_timestamp", kwargs))

def mount_table(path, **kwargs):
    clear_metadata_caches(kwargs.get("driver"))
    kwargs["path"] = path
    return execute_command("mount_table", kwargs)

def unmount_table(path, **kwargs):
    clear_metadata_caches(kwargs.get("driver"))
    kwargs["path"] = path
    return execute_command("unmount_table", kwargs)

def remount_table(path, **kwargs):
    kwargs["path"] = path
    return execute_command("remount_table", kwargs)

def freeze_table(path, **kwargs):
    clear_metadata_caches(kwargs.get("driver"))
    kwargs["path"] = path
    return execute_command("freeze_table", kwargs)

def unfreeze_table(path, **kwargs):
    clear_metadata_caches(kwargs.get("driver"))
    kwargs["path"] = path
    return execute_command("unfreeze_table", kwargs)

def reshard_table(path, arg, **kwargs):
    clear_metadata_caches(kwargs.get("driver"))
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

class EventsOnFs(object):
    """ EventsOnFs helps to exchange information between test code
    and test MR jobs about different events."""
    def __init__(self, label="eventdir"):
        self._tmpdir = create_tmpdir(label)

    def notify_event(self, event_name):
        file_name = self._get_event_filename(event_name)
        print >>sys.stderr, "touching", file_name
        with open(file_name, 'w'):
            pass

    def notify_event_cmd(self, event_name):
        return "touch '{0}'".format(self._get_event_filename(event_name))

    def wait_event(self, event_name, timeout=timedelta(seconds=60)):
        file_name = self._get_event_filename(event_name)
        deadline = datetime.now() + timeout
        while True:
            if os.path.exists(file_name):
                break
            if datetime.now() > deadline:
                raise TimeoutError("Timeout exceeded while waiting for {0}".format(event_name))
            time.sleep(0.1)

    def wait_event_cmd(self, event_name, timeout=timedelta(seconds=60)):
        return (
            " {{ wait_limit={wait_limit}\n"
            " while ! [ -f {event_file_name} ]\n"
            " do\n"
            "     sleep 0.1 ; ((wait_limit--)) ;\n"
            "     if [ $wait_limit -le 0 ] ; then \n"
            "         echo timeout for event {event_name} exceeded >&2 ; exit 1 ;\n"
            "     fi\n"
            " done \n"
            "}}").format(
                event_name=event_name,
                event_file_name=self._get_event_filename(event_name),
                wait_limit=timeout.seconds*10)

    def _get_event_filename(self, event_name):
        if not event_name:
            raise ValueError("event_name must be non empty")
        return os.path.join(self._tmpdir, event_name)


class Operation(object):
    def __init__(self):
        self.resumed_jobs = __builtin__.set()

        self._tmpdir = ""
        self._poll_frequency = 0.1

    def get_job_phase(self, job_id):
        job_path = "//sys/scheduler/orchid/scheduler/job_by_id/{0}".format(job_id)
        node = get(job_path + "/address", verbose=False)
        job_phase_path = "//sys/nodes/{0}/orchid/job_controller/active_jobs/scheduler/{1}/job_phase".format(node, job_id)
        return get(job_phase_path, verbose=False)

    def wait_for_running_phase(self, timeout=5.0):
        while True:
            wait = False
            for job in self.jobs:
                try:
                    if self.get_job_phase(job) != "running":
                        wait = True
                except YtResponseError as error:
                    if error.is_resolve_error():
                        continue
                    raise

            if wait == False:
                break

            if timeout <= 0:
                TimeoutError("Job phase didn't become running within timeout")

            print >>sys.stderr, "Some jobs are not in running phase yet, waiting..."
            time.sleep(self._poll_frequency)
            timeout -= self._poll_frequency

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
                progress = get(progress_path + "/jobs", verbose=False, verbose_error=False)
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

        self.wait_for_running_phase()

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
        abort_op(self.id, **kwargs)

    def complete(self, **kwargs):
        kwargs["operation_id"] = self.id
        execute_command("complete_op", kwargs)

    def suspend(self, **kwargs):
        suspend_op(self.id, **kwargs)

    def resume(self, **kwargs):
        resume_op(self.id, **kwargs)

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
            "while [ -f {0}/started_$YT_JOB_ID ]; do sleep 0.1; done\n"
            "{3}\n)"
            .format(
                operation._tmpdir,
                kwargs.get("precommand", ""),
                kwargs["command"],
                kwargs.get("postcommand", "")))

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
        wait_timeout = kwargs.get("wait_timeout", 20)
        if "wait_timeout" in kwargs:
            del kwargs["wait_timeout"]
        operation.ensure_jobs_running(wait_timeout)

    if track:
        operation.track()

    return operation

def abort_op(op_id, **kwargs):
    kwargs["operation_id"] = op_id
    execute_command("abort_op", kwargs)

def suspend_op(op_id, **kwargs):
    kwargs["operation_id"] = op_id
    execute_command("suspend_op", kwargs)

def resume_op(op_id, **kwargs):
    kwargs["operation_id"] = op_id
    execute_command("resume_op", kwargs)

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

def gc_collect(driver=None):
    _get_driver(driver=driver).gc_collect()

def clear_metadata_caches(driver=None):
    _get_driver(driver=driver).clear_metadata_caches()

def create_account(name, **kwargs):
    kwargs["type"] = "account"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_account(name, **kwargs):
    remove("//sys/accounts/" + name, **kwargs)
    gc_collect(kwargs.get("driver"))

def create_user(name, **kwargs):
    kwargs["type"] = "user"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_user(name, **kwargs):
    remove("//sys/users/" + name, **kwargs)
    gc_collect(kwargs.get("driver"))

def create_group(name, **kwargs):
    kwargs["type"] = "group"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_group(name, **kwargs):
    remove("//sys/groups/" + name, **kwargs)
    gc_collect(kwargs.get("driver"))

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

def remove_tablet_cell_bundle(name, driver=None):
    remove("//sys/tablet_cell_bundles/" + name, driver=driver)
    gc_collect(driver=driver)

def remove_tablet_cell(id, driver=None):
    remove("//sys/tablet_cells/" + id, driver=driver)
    gc_collect(driver=driver)

def create_table_replica(table_path, cluster_name, replica_path, **kwargs):
    kwargs["type"] = "table_replica"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["table_path"] = table_path
    kwargs["attributes"]["cluster_name"] = cluster_name
    kwargs["attributes"]["replica_path"] = replica_path
    return yson.loads(execute_command("create", kwargs))

def remove_table_replica(replica_id):
    remove("#{0}".format(replica_id))

def enable_table_replica(replica_id, **kwargs):
    kwargs["replica_id"] = replica_id
    execute_command("enable_table_replica", kwargs)

def disable_table_replica(replica_id, **kwargs):
    kwargs["replica_id"] = replica_id
    execute_command("disable_table_replica", kwargs)

def create_data_center(name, **kwargs):
    kwargs["type"] = "data_center"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_data_center(name, **kwargs):
    remove("//sys/data_centers/" + name, **kwargs)
    gc_collect(kwargs.get("driver"))

def create_rack(name, **kwargs):
    kwargs["type"] = "rack"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

def remove_rack(name, **kwargs):
    remove("//sys/racks/" + name, **kwargs)
    gc_collect(kwargs.get("driver"))

def create_medium(name, **kwargs):
    kwargs["type"] = "medium"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)

#########################################
# Helpers:

def get_transactions(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/transactions", driver=driver)

def get_topmost_transactions(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/topmost_transactions", driver=driver)

def get_chunks(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/chunks", driver=driver)

def get_accounts(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/accounts", driver=driver)

def get_users(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/users", driver=driver)

def get_groups(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/groups", driver=driver)

def get_tablet_cells(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/tablet_cells", driver=driver)

def get_data_centers(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/data_centers", driver=driver)

def get_racks(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/racks", driver=driver)

def get_nodes(driver=None):
    return ls("//sys/nodes", driver=driver)

def get_media(driver=None):
    gc_collect(driver=driver)
    return ls("//sys/media", driver=driver)

def get_chunk_owner_disk_space(path, *args, **kwargs):
    disk_space = get("{0}/@resource_usage/disk_space_per_medium".format(path), *args, **kwargs)
    return disk_space.get("default", 0)

def get_recursive_disk_space(path):
    disk_space = get("{0}/@recursive_resource_usage/disk_space_per_medium".format(path))
    return disk_space.get("default", 0)

def get_account_disk_space(account):
    disk_space = get("//sys/accounts/{0}/@resource_usage/disk_space_per_medium".format(account))
    return disk_space.get("default", 0)

def get_account_committed_disk_space(account):
    disk_space = get("//sys/accounts/{0}/@committed_resource_usage/disk_space_per_medium".format(account))
    return disk_space.get("default", 0)

def get_account_disk_space_limit(account):
    disk_space = get("//sys/accounts/{0}/@resource_limits/disk_space_per_medium".format(account))
    return disk_space.get("default", 0)

def set_account_disk_space_limit(account, limit):
    set("//sys/accounts/{0}/@resource_limits/disk_space_per_medium/default".format(account), limit)

def get_chunk_replication_factor(chunk_id):
    return get("#{0}/@media/default/replication_factor".format(chunk_id))

#########################################

def get_last_profiling_values(orchid_path, metrics):
    # To ensure that profiling updated.
    time.sleep(1)

    values = {}
    for metric in metrics:
        values[metric] = get(orchid_path + "/" + metric, verbose=False)[-1]["value"]
    return values

def make_schema(columns, **attributes):
    schema = yson.YsonList(columns)
    for attr, value in attributes.items():
        schema.attributes[attr] = value
    return schema

def make_ace(action, subjects, permissions, inheritance_mode="object_and_descendants"):
    def _to_list(x):
        if isinstance(x, str):
            return [x]
        else:
            return x
    return {
        "action": action,
        "subjects": _to_list(subjects),
        "permissions": _to_list(permissions),
        "inheritance_mode": inheritance_mode
    }

#########################################

def total_seconds(td):
    return float(td.microseconds + (td.seconds + td.days * 24 * 3600) * 10 ** 6) / 10 ** 6
