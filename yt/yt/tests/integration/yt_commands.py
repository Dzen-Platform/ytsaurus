from __future__ import print_function

import yt_scheduler_helpers

from yt.environment.yt_env import set_environment_driver_logging_config

import yt.yson as yson
from yt_driver_bindings import Driver, Request
from yt.common import (
    YtError,
    YtResponseError,
    flatten,
    update_inplace,
    update,
    date_string_to_datetime,
    uuid_to_parts,
)
from yt.ypath import parse_ypath

from yt.packages.six import itervalues, iteritems, iterkeys
from yt.packages.six.moves import xrange, builtins

from yt.test_helpers import wait, WaitFailed
from yt.test_helpers.job_events import JobEvents, TimeoutError

import contextlib
import copy as pycopy
import os
import pytest
import random
import stat
import sys
import tempfile
import time
import traceback
import logging
try:
    from string import letters as ascii_letters
except ImportError:
    from string import ascii_letters

from datetime import datetime, timedelta
try:
    from cStringIO import StringIO as BytesIO, OutputType
except ImportError:  # Python 3
    from io import BytesIO
    OutputType = BytesIO

###########################################################################

root_logger = logging.getLogger()

clusters_drivers = {}
is_multicell = None
path_to_run_tests = None
_zombie_responses = []
_events_on_fs = None
default_api_version = 4

# See transaction_client/public.h
SyncLastCommittedTimestamp = 0x3FFFFFFFFFFFFF01
AsyncLastCommittedTimestamp = 0x3FFFFFFFFFFFFF04
MinTimestamp = 0x0000000000000001


def authors(*the_authors):
    return pytest.mark.authors(the_authors)


@contextlib.contextmanager
def raises_yt_error(code=None, required=True):
    """
    Context manager that helps to check that code raises YTError.
    When description is int we check that raised error contains this error code.
    When description is string we check that raised error contains description as substring.
    Value of context manager is a single-element list containing caught error.

    Examples:
        with raises_yt_error(yt_error_codes.SortOrderViolation):
            ...

        with raises_yt_error("Name of struct field #0 is empty"):
            ...

        with raises_yt_error() as err:
            ...
        assert err[0].contains_code(42) and len(err[0].inner_errors) > 0
    """

    result_list = []
    if not isinstance(code, (str, int, type(None))):
        raise TypeError("code must be str, int or None, actual type: {}".format(code.__class__))
    try:
        yield result_list
        if required:
            raise AssertionError("Expected exception to be raised")
    except YtError as e:
        if isinstance(code, int):
            if not e.contains_code(code):
                raise AssertionError(
                    "Raised error doesn't contain error code {}:\n{}".format(
                        code,
                        e,
                    )
                )
        elif isinstance(code, str):
            if code not in str(e):
                raise AssertionError(
                    "Raised error doesn't contain {}:\n{}".format(
                        code,
                        e,
                    )
                )
        else:
            assert code is None
        result_list.append(e)


def print_debug(*args):
    if args:
        root_logger.debug(" ".join(builtins.map(str, args)))


def get_driver(cell_index=0, cluster="primary", api_version=default_api_version):
    if cluster not in clusters_drivers:
        return None

    return clusters_drivers[cluster][cell_index][api_version]


def _get_driver(driver):
    if driver is None:
        return get_driver()
    else:
        return driver


def init_drivers(clusters):
    def create_driver_per_api(config):
        drivers = {}
        for api_version in (3, 4):
            config = pycopy.deepcopy(config)
            config["api_version"] = api_version
            drivers[api_version] = Driver(config=config)
        return drivers

    for instance in clusters:
        if instance.yt_config.master_count > 0:
            if instance._default_driver_backend == "native":
                default_driver = create_driver_per_api(instance.configs["driver"])
            else:
                default_driver = create_driver_per_api(instance.configs["rpc_driver"])

            # Setup driver logging for all instances in the environment as in the primary cluster.
            if instance._cluster_name == "primary":
                set_environment_driver_logging_config(instance.configs["driver_logging"])

            secondary_drivers = [
                create_driver_per_api(instance.configs["driver_secondary_" + str(i)])
                for i in xrange(instance.yt_config.secondary_cell_count)
            ]

            clusters_drivers[instance._cluster_name] = [default_driver] + secondary_drivers


def sorted_dicts(list_of_dicts):
    return sorted(list_of_dicts, key=lambda dict: sorted(dict.items()))


def wait_assert(check_fn, *args, **kwargs):
    last_exception = []
    last_exc_info = []

    def wrapper():
        try:
            check_fn(*args, **kwargs)
        except AssertionError as e:
            last_exception[:] = [e]
            last_exc_info[:] = [sys.exc_info()]
            print_debug("Assertion failed, retrying\n{}".format(e))
            return False
        return True

    try:
        wait(wrapper)
    except WaitFailed:
        if not last_exception:
            raise
        tb = "\n".join(traceback.format_tb(last_exc_info[0][2]))
        raise AssertionError("waited assertion failed\n{}{}".format(tb, last_exception[0]))


def wait_drivers():
    for cluster in list(clusters_drivers.values()):
        cell_tag = 0

        def driver_is_ready():
            try:
                return get("//@", driver=cluster[cell_tag][default_api_version])
            except Exception:
                root_logger.exception("Checking driver is ready failed")
                return False

        wait(driver_is_ready, ignore_exceptions=True)


def terminate_drivers():
    for cluster in clusters_drivers:
        for drivers_by_cell_tag in clusters_drivers[cluster]:
            for driver in list(itervalues(drivers_by_cell_tag)):
                driver.terminate()
    clusters_drivers.clear()


def get_branch(dict, path):
    root = dict
    for field in path:
        assert isinstance(field, str), "non-string keys are not allowed in command parameters"
        if field not in root:
            return None
        root = root[field]
    return root


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


def get_at(obj, path, default):
    parts = path.lstrip("/").split("/")
    for part in parts[:-1]:
        if isinstance(obj, dict):
            obj = obj.get(part, {})
        elif isinstance(obj, list):
            if int(part) < len(obj):
                obj = obj[int(part)]
            else:
                obj = {}
        else:
            raise RuntimeError("Failed to get at path (obj: {}, path: {})".format(obj, path))

    last_part = parts[-1]
    if isinstance(obj, dict):
        obj = obj.get(last_part, default)
    elif isinstance(obj, list):
        if int(part) < len(obj):
            obj = obj[int(part)]
        else:
            obj = default
    else:
        raise RuntimeError("Failed to get at path (obj: {}, path: {})".format(obj, path))

    return obj


def prepare_path(path):
    if not path:
        return path
    attributes = {}
    if isinstance(path, yson.YsonType):
        attributes = path.attributes
    result = parse_ypath(path)
    update_inplace(result.attributes, attributes)
    return result


def prepare_paths(paths):
    return [prepare_path(path) for path in flatten(paths)]


def prepare_parameters(parameters):
    change(parameters, "tx", "transaction_id")
    change(parameters, "ping_ancestor_txs", "ping_ancestor_transactions")
    return parameters


def retry(
    func,
    retry_timeout=timedelta(seconds=10),
    retry_interval=timedelta(milliseconds=100),
):
    now = datetime.now()
    deadline = now + retry_timeout
    while now < deadline:
        try:
            return func()
        except:  # noqa
            time.sleep(retry_interval.total_seconds())
            now = datetime.now()
    return func()


def execute_command(
    command_name,
    parameters,
    input_stream=None,
    output_stream=None,
    verbose=None,
    verbose_error=None,
    ignore_result=False,
    return_response=False,
    parse_yson=None,
    unwrap_v4_result=True,
):
    global _zombie_responses

    if "verbose" in parameters:
        verbose = parameters["verbose"]
        del parameters["verbose"]
    verbose = verbose is None or verbose

    if "verbose_error" in parameters:
        verbose_error = parameters["verbose_error"]
        del parameters["verbose_error"]
    verbose_error = verbose_error is None or verbose_error

    if "ignore_result" in parameters:
        ignore_result = parameters["ignore_result"]
        del parameters["ignore_result"]
    ignore_result = ignore_result is None or ignore_result

    if "return_response" in parameters:
        return_response = parameters["return_response"]
        del parameters["return_response"]
    return_response = return_response is None or return_response

    driver = _get_driver(parameters.pop("driver", None))

    command_rewrites = {
        "start_tx": "start_transaction",
        "abort_tx": "abort_transaction",
        "commit_tx": "commit_transaction",
        "ping_tx": "ping_transaction",
        "start_op": "start_operation",
        "abort_op": "abort_operation",
        "suspend_op": "suspend_operation",
        "resume_op": "resume_operation",
        "complete_op": "complete_operation",
        "update_op_parameters": "update_operation_parameters",
    }
    if driver.get_config()["api_version"] == 4 and command_name in command_rewrites:
        command_name = command_rewrites[command_name]

    if command_name in (
        "merge",
        "erase",
        "map",
        "sort",
        "reduce",
        "join_reduce",
        "map_reduce",
        "remote_copy",
    ):
        parameters["operation_type"] = command_name
        command_name = "start_operation"

    authenticated_user = None
    if "authenticated_user" in parameters:
        authenticated_user = parameters["authenticated_user"]
        del parameters["authenticated_user"]

    if "rewrite_operation_path" not in parameters:
        parameters["rewrite_operation_path"] = False

    if "path" in parameters:
        parameters["path"] = prepare_path(parameters["path"])

    if "paths" in parameters:
        parameters["paths"] = yson.loads(parameters["paths"].encode("ascii"))
        for index in range(len(parameters["paths"])):
            parameters["paths"][index] = prepare_path(parameters["paths"][index])

    response_parameters = parameters.pop("response_parameters", None)

    yson_format = yson.to_yson_type("yson", attributes={"format": "text"})

    description = driver.get_command_descriptor(command_name)
    if description.input_type() != "null" and parameters.get("input_format") is None:
        parameters["input_format"] = yson_format
    if description.output_type() != "null":
        if parameters.get("output_format") is None:
            parameters["output_format"] = yson_format
        if output_stream is None:
            output_stream = BytesIO()

    parameters = prepare_parameters(parameters)

    if verbose:

        def _is_text_yson(fmt):
            return fmt is not None and str(fmt) == "yson" and fmt.attributes.get("format", None) == "text"

        pretty_parameters = pycopy.deepcopy(parameters)
        for key in ["input_format", "output_format"]:
            if _is_text_yson(pretty_parameters.get(key, None)):
                pretty_parameters.pop(key)

        print_debug(str(datetime.now()), command_name, pretty_parameters)

    response = driver.execute(
        Request(
            command_name=command_name,
            parameters=parameters,
            input_stream=input_stream,
            output_stream=output_stream,
            user=authenticated_user,
        )
    )

    if ignore_result:
        _zombie_responses.append(response)
        return

    if return_response:
        return response

    response.wait()

    if response_parameters is not None:
        response_params = response.response_parameters()
        print_debug(response_parameters)
        response_parameters.update(response_params)

    if not response.is_ok():
        # TODO(ignat): it build empty error with response.error() as inner error. Fix it!
        error = YtResponseError(response.error())
        if verbose_error:
            print_debug(str(error))
            # NB: we want to see inner errors in teamcity.
        raise error
    if isinstance(output_stream, OutputType):
        result = output_stream.getvalue()
        if verbose:
            print_debug(result)
        if parse_yson:
            result = yson.loads(result)
            if (
                unwrap_v4_result
                and driver.get_config()["api_version"] == 4
                and isinstance(result, dict)
                and len(list(iterkeys(result))) == 1
            ):
                result = list(result.values())[0]
            if driver.get_config()["api_version"] == 3 and command_name == "lock":
                result = {"lock_id": result}
        return result


def execute_command_with_output_format(command_name, kwargs, input_stream=None):
    has_output_format = "output_format" in kwargs
    return_response = "return_response" in kwargs
    if not has_output_format:
        kwargs["output_format"] = yson.loads(b"<format=text>yson")
    output = kwargs.pop("output_stream", BytesIO())
    response = execute_command(command_name, kwargs, input_stream=input_stream, output_stream=output)
    if return_response:
        return response
    if not has_output_format:
        return list(yson.loads(output.getvalue(), yson_type="list_fragment"))
    else:
        return output.getvalue()


###########################################################################


def _assert_true_for_cell(cell_index, predicate):
    assert predicate(get_driver(cell_index))


def assert_true_for_secondary_cells(env, predicate):
    for i in xrange(env.yt_config.secondary_cell_count):
        _assert_true_for_cell(i + 1, predicate)


def assert_true_for_all_cells(env, predicate):
    _assert_true_for_cell(0, predicate)
    assert_true_for_secondary_cells(env, predicate)


def _check_true_for_all_cells(env, predicate):
    for i in xrange(env.yt_config.secondary_cell_count + 1):
        if not predicate(get_driver(i)):
            return False
    return True


def wait_true_for_all_cells(env, predicate):
    wait(lambda: _check_true_for_all_cells(env, predicate))


###########################################################################


def multicell_sleep():
    if is_multicell:
        time.sleep(0.5)


def master_memory_sleep():
    multicell_sleep()
    time.sleep(0.2)
    multicell_sleep()


def dump_job_context(job_id, path, **kwargs):
    kwargs["job_id"] = job_id
    kwargs["path"] = path
    return execute_command("dump_job_context", kwargs)


def get_job_input(job_id, **kwargs):
    kwargs["job_id"] = job_id
    return execute_command("get_job_input", kwargs)


def get_job_input_paths(job_id, **kwargs):
    kwargs["job_id"] = job_id
    return execute_command("get_job_input_paths", kwargs)


def get_job_spec(job_id, **kwargs):
    kwargs["job_id"] = job_id
    return execute_command("get_job_spec", kwargs)


def get_table_columnar_statistics(paths, **kwargs):
    kwargs["paths"] = paths
    return execute_command("get_table_columnar_statistics", kwargs, parse_yson=True)


def get_job_stderr(operation_id, job_id, **kwargs):
    kwargs["operation_id"] = operation_id
    kwargs["job_id"] = job_id
    return execute_command("get_job_stderr", kwargs)


def get_job_fail_context(operation_id, job_id, **kwargs):
    kwargs["operation_id"] = operation_id
    kwargs["job_id"] = job_id
    return execute_command("get_job_fail_context", kwargs)


def list_operations(**kwargs):
    return execute_command("list_operations", kwargs, parse_yson=True)


def list_jobs(operation_id, **kwargs):
    kwargs["operation_id"] = operation_id
    return execute_command("list_jobs", kwargs, parse_yson=True)


def abandon_job(job_id, **kwargs):
    kwargs["job_id"] = job_id
    execute_command("abandon_job", kwargs)


def poll_job_shell(job_id, authenticated_user=None, shell_name=None, **kwargs):
    kwargs = {"job_id": job_id, "parameters": kwargs}
    if authenticated_user:
        kwargs["authenticated_user"] = authenticated_user
    if shell_name:
        kwargs["shell_name"] = shell_name
    return execute_command("poll_job_shell", kwargs, parse_yson=True)


def abort_job(job_id, **kwargs):
    kwargs["job_id"] = job_id
    execute_command("abort_job", kwargs)


def interrupt_job(job_id, interrupt_timeout=10000, **kwargs):
    kwargs["job_id"] = job_id
    kwargs["interrupt_timeout"] = interrupt_timeout
    execute_command("abort_job", kwargs)


def retry_while_job_missing(func):
    result = [None]

    def check():
        try:
            result[0] = func()
            return True
        except YtError as err:
            if err.is_no_such_job():
                return False
            raise

    wait(check)
    return result[0]


def lock(path, waitable=False, **kwargs):
    kwargs["path"] = path
    kwargs["waitable"] = waitable
    return execute_command("lock", kwargs, parse_yson=True)


def unlock(path, **kwargs):
    kwargs["path"] = path
    execute_command("unlock", kwargs)


def remove(path, **kwargs):
    kwargs["path"] = path
    execute_command("remove", kwargs)


def get(path, is_raw=False, **kwargs):
    kwargs["path"] = path
    if "default" in kwargs and "verbose_error" not in kwargs:
        kwargs["verbose_error"] = False
    if "return_only_value" not in kwargs:
        kwargs["return_only_value"] = True
    try:
        return execute_command("get", kwargs, parse_yson=not is_raw, unwrap_v4_result=False)
    except YtResponseError as err:
        if err.is_resolve_error() and "default" in kwargs:
            return kwargs["default"]
        raise


def get_job(operation_id, job_id, **kwargs):
    kwargs["operation_id"] = operation_id
    kwargs["job_id"] = job_id
    return execute_command("get_job", kwargs, parse_yson=True)


def set(path, value, is_raw=False, **kwargs):
    if not is_raw:
        value = yson.dumps(value)
    kwargs["path"] = path
    execute_command("set", kwargs, input_stream=BytesIO(value))


def multiset_attributes(path, subrequests, is_raw=False, **kwargs):
    if not is_raw:
        subrequests = yson.dumps(subrequests)
    kwargs["path"] = path
    execute_command("multiset_attributes", kwargs, input_stream=BytesIO(subrequests))


def create(object_type, path, **kwargs):
    kwargs["type"] = object_type
    kwargs["path"] = path
    return execute_command("create", kwargs, parse_yson=True)


def copy(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return execute_command("copy", kwargs, parse_yson=True)


def move(source_path, destination_path, **kwargs):
    kwargs["source_path"] = source_path
    kwargs["destination_path"] = destination_path
    return execute_command("move", kwargs, parse_yson=True)


def link(target_path, link_path, **kwargs):
    kwargs["target_path"] = target_path
    kwargs["link_path"] = link_path
    return execute_command("link", kwargs, parse_yson=True)


def exists(path, **kwargs):
    kwargs["path"] = path
    return execute_command("exists", kwargs, parse_yson=True)


def concatenate(source_paths, destination_path, **kwargs):
    kwargs["source_paths"] = source_paths
    kwargs["destination_path"] = destination_path
    execute_command("concatenate", kwargs)


def externalize(path, cell_tag, **kwargs):
    kwargs["path"] = path
    kwargs["cell_tag"] = cell_tag
    execute_command("externalize", kwargs)


def internalize(path, **kwargs):
    kwargs["path"] = path
    execute_command("internalize", kwargs)


def ls(path, **kwargs):
    kwargs["path"] = path
    return execute_command("list", kwargs, parse_yson=True)


def read_table(path, **kwargs):
    kwargs["path"] = path
    return execute_command_with_output_format("read_table", kwargs)


def read_blob_table(path, **kwargs):
    kwargs["path"] = path
    output = BytesIO()
    execute_command("read_blob_table", kwargs, output_stream=output)
    return output.getvalue()


def write_table(path, value=None, is_raw=False, **kwargs):
    if "input_stream" in kwargs:
        input_stream = kwargs.pop("input_stream")
    else:
        assert value is not None
        if not is_raw:
            if not isinstance(value, list):
                value = [value]
            value = yson.dumps(value, yson_type="list_fragment")
        input_stream = BytesIO(value)

    attributes = {}
    if "sorted_by" in kwargs:
        attributes["sorted_by"] = flatten(kwargs["sorted_by"])
        del kwargs["sorted_by"]
    kwargs["path"] = yson.to_yson_type(path, attributes=attributes)
    return execute_command("write_table", kwargs, input_stream=input_stream)


def locate_skynet_share(path, **kwargs):
    kwargs["path"] = path

    output = BytesIO()
    execute_command("locate_skynet_share", kwargs, output_stream=output)
    return yson.loads(output.getvalue())


def select_rows(query, **kwargs):
    kwargs["query"] = query
    kwargs["verbose_logging"] = True
    return execute_command_with_output_format("select_rows", kwargs)


def explain_query(query, **kwargs):
    kwargs["query"] = query
    return execute_command_with_output_format("explain_query", kwargs)[0]


def _prepare_rows_stream(data, is_raw=False):
    # remove surrounding [ ]
    if not is_raw:
        data = yson.dumps(data, yson_type="list_fragment")
    return BytesIO(data)


def insert_rows(path, data, is_raw=False, **kwargs):
    kwargs["path"] = path
    if not is_raw:
        return execute_command("insert_rows", kwargs, input_stream=_prepare_rows_stream(data))
    else:
        return execute_command("insert_rows", kwargs, input_stream=BytesIO(data))


def lock_rows(path, data, **kwargs):
    kwargs["path"] = path
    return execute_command("lock_rows", kwargs, input_stream=_prepare_rows_stream(data))


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


def pull_rows(path, **kwargs):
    kwargs["path"] = path
    return execute_command_with_output_format("pull_rows", kwargs)


def get_in_sync_replicas(path, data, **kwargs):
    kwargs["path"] = path
    if data is None:
        kwargs["all_keys"] = True
        data = []
    return execute_command(
        "get_in_sync_replicas",
        kwargs,
        input_stream=_prepare_rows_stream(data),
        parse_yson=True,
    )


def get_table_pivot_keys(path, **kwargs):
    kwargs["path"] = path
    return execute_command("get_table_pivot_keys", kwargs, parse_yson=True)


def get_tablet_infos(path, tablet_indexes, **kwargs):
    kwargs["path"] = path
    kwargs["tablet_indexes"] = tablet_indexes
    return execute_command("get_tablet_infos", kwargs, parse_yson=True, unwrap_v4_result=False)


def start_transaction(**kwargs):
    return execute_command("start_tx", kwargs, parse_yson=True)


def commit_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    execute_command("commit_tx", kwargs)


def ping_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    execute_command("ping_tx", kwargs)


def abort_transaction(tx, **kwargs):
    kwargs["transaction_id"] = tx
    execute_command("abort_tx", kwargs)


def abort_all_transactions():
    topmost_transactions = ls("//sys/topmost_transactions")
    for i in xrange(len(topmost_transactions) / 100 + 1):
        start = i * 100
        end = min(len(topmost_transactions), (i + 1) * 100)
        if start >= end:
            break
        requests = []
        for j in xrange(start, end):
            requests.append(
                {
                    "command": "abort_transaction",
                    "parameters": {"transaction_id": topmost_transactions[j]},
                }
            )
        execute_batch(requests)


def generate_timestamp(**kwargs):
    return execute_command("generate_timestamp", kwargs, parse_yson=True)


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


def reshard_table(path, arg=None, **kwargs):
    clear_metadata_caches(kwargs.get("driver"))
    kwargs["path"] = path
    assert arg is not None
    if isinstance(arg, int):
        kwargs["tablet_count"] = arg
    else:
        kwargs["pivot_keys"] = arg
    return execute_command("reshard_table", kwargs)


def reshard_table_automatic(path, **kwargs):
    clear_metadata_caches(kwargs.get("driver"))
    kwargs["path"] = path
    return execute_command("reshard_table_automatic", kwargs, parse_yson=True)


def alter_table(path, **kwargs):
    kwargs["path"] = path
    return execute_command("alter_table", kwargs)


def balance_tablet_cells(bundle, tables=None, **kwargs):
    kwargs["bundle"] = bundle
    if tables is not None:
        kwargs["tables"] = tables
    return execute_command("balance_tablet_cells", kwargs, parse_yson=True)


def _parse_backup_manifest(*args):
    def _make_table_manifest(x):
        return {"source_path": x[0], "destination_path": x[1]}

    if type(args[0]) == list:
        return {
            "clusters": {
                "primary": builtins.map(_make_table_manifest, args),
            }
        }
    else:
        return {
            "clusters": {
                cluster: builtins.map(_make_table_manifest, tables)
                for cluster, tables
                in list(iteritems(args))
            }
        }


def create_table_backup(*args):
    return execute_command("create_table_backup", {"manifest": _parse_backup_manifest(*args)})


def restore_table_backup(*args):
    return execute_command("restore_table_backup", {"manifest": _parse_backup_manifest(*args)})


def write_file(path, data, **kwargs):
    kwargs["path"] = path

    if "input_stream" in kwargs:
        assert data is None
        input_stream = kwargs["input_stream"]
        del kwargs["input_stream"]
    else:
        input_stream = BytesIO(data)

    return execute_command("write_file", kwargs, input_stream=input_stream)


def write_local_file(path, file_name, **kwargs):
    with open(file_name, "rt") as f:
        return write_file(path, f.read(), **kwargs)


def read_file(path, **kwargs):
    kwargs["path"] = path
    output = BytesIO()
    execute_command("read_file", kwargs, output_stream=output)
    return output.getvalue()


def read_journal(path, **kwargs):
    kwargs["path"] = path
    kwargs["output_format"] = yson.to_yson_type("yson")
    output = BytesIO()
    execute_command("read_journal", kwargs, output_stream=output)
    return list(yson.loads(output.getvalue(), yson_type="list_fragment"))


def write_journal(path, rows=None, input_stream=None, is_raw=False, **kwargs):
    kwargs["path"] = path

    if input_stream is not None:
        assert rows is None
    elif rows is not None:
        assert input_stream is None
        yson_rows = yson.dumps(rows, yson_type="list_fragment")
        input_stream = BytesIO(yson_rows)
    else:
        assert False

    return execute_command("write_journal", kwargs, input_stream=input_stream)


def make_batch_request(command_name, input=None, **kwargs):
    request = dict()
    request["command"] = command_name
    request["parameters"] = kwargs
    if input is not None:
        request["input"] = input
    return request


def execute_batch(requests, **kwargs):
    kwargs["requests"] = requests
    return execute_command("execute_batch", kwargs, parse_yson=True)


def get_batch_error(result):
    if "error" in result:
        return result["error"]
    else:
        return None


def get_batch_output(result):
    if "error" in result:
        raise YtResponseError(result["error"])
    if "output" in result:
        return result["output"]
    return None


def raise_batch_error(result):
    if "error" in result:
        raise YtResponseError(result["error"])


def check_permission(user, permission, path, **kwargs):
    kwargs["user"] = user
    kwargs["permission"] = permission
    kwargs["path"] = path
    return execute_command("check_permission", kwargs, parse_yson=True, unwrap_v4_result=False)


def check_permission_by_acl(user, permission, acl, **kwargs):
    kwargs["user"] = user
    kwargs["permission"] = permission
    kwargs["acl"] = acl
    return execute_command("check_permission_by_acl", kwargs, parse_yson=True, unwrap_v4_result=False)


def transfer_account_resources(source_account, destination_account, resource_delta, **kwargs):
    kwargs["source_account"] = source_account
    kwargs["destination_account"] = destination_account
    kwargs["resource_delta"] = resource_delta
    execute_command("transfer_account_resources", kwargs)


def transfer_pool_resources(source_pool, destination_pool, pool_tree, resource_delta, **kwargs):
    kwargs["source_pool"] = source_pool
    kwargs["destination_pool"] = destination_pool
    kwargs["pool_tree"] = pool_tree
    kwargs["resource_delta"] = resource_delta
    execute_command("transfer_pool_resources", kwargs)


def get_file_from_cache(md5, cache_path, **kwargs):
    kwargs["md5"] = md5
    kwargs["cache_path"] = cache_path
    return execute_command("get_file_from_cache", kwargs, parse_yson=True)


def put_file_to_cache(path, md5, **kwargs):
    kwargs["path"] = path
    kwargs["md5"] = md5
    return execute_command("put_file_to_cache", kwargs, parse_yson=True)


def discover_proxies(type_, **kwargs):
    kwargs["type"] = type_
    return execute_command("discover_proxies", kwargs, parse_yson=True)


def get_supported_features(**kwargs):
    return execute_command("get_supported_features", kwargs, parse_yson=True)


###########################################################################


def reset_events_on_fs():
    global _events_on_fs
    _events_on_fs = None


def events_on_fs():
    global _events_on_fs
    if _events_on_fs is None:
        _events_on_fs = JobEvents(create_tmpdir("eventdir"))
    return _events_on_fs


def with_breakpoint(cmd, breakpoint_name="default"):
    if "BREAKPOINT" not in cmd:
        raise ValueError("Command doesn't have BREAKPOINT: {0}".format(cmd))
    result = cmd.replace("BREAKPOINT", events_on_fs().breakpoint_cmd(breakpoint_name), 1)
    if "BREAKPOINT" in result:
        raise ValueError("Command has multiple BREAKPOINT: {0}".format(cmd))
    return result


def wait_breakpoint(*args, **kwargs):
    return events_on_fs().wait_breakpoint(*args, **kwargs)


def release_breakpoint(*args, **kwargs):
    return events_on_fs().release_breakpoint(*args, **kwargs)


###########################################################################


def get_operation_cypress_path(op_id):
    return "//sys/operations/{}/{}".format("%02x" % (int(op_id.split("-")[3], 16) % 256), op_id)


def get_cypress_metrics(operation_id, key, aggr="sum"):
    statistics = get(get_operation_cypress_path(operation_id) + "/@progress/job_statistics")
    return sum(
        filter(
            lambda x: x is not None,
            [
                get_statistics(statistics, "{0}.$.{1}.map.{2}".format(key, job_state, aggr))
                for job_state in ("completed", "failed", "aborted")
            ],
        )
    )


##################################################################


class Operation(object):
    def __init__(self):
        self._tmpdir = ""
        self._poll_frequency = 0.1

    def get_path(self):
        return get_operation_cypress_path(self.id)

    def get_node(self, job_id):
        job_path = "//sys/scheduler/orchid/scheduler/jobs/{0}".format(job_id)
        return get(job_path + "/address", verbose=False)

    def get_job_phase(self, job_id):
        job_phase_path = "//sys/cluster_nodes/{0}/orchid/job_controller/active_jobs/scheduler/{1}/job_phase".format(
            self.get_node(job_id), job_id
        )
        return get(job_phase_path, verbose=False)

    def ensure_running(self, timeout=10.0):
        print_debug("Waiting for operation %s to become running" % self.id)

        state = self.get_runtime_state(verbose=False)
        while state != "running" and timeout > 0:
            time.sleep(self._poll_frequency)
            timeout -= self._poll_frequency
            state = self.get_state(verbose=False)

        if state != "running":
            raise TimeoutError("Operation didn't become running within timeout")

        wait(
            lambda: self.get_state() == "running",
            sleep_backoff=self._poll_frequency,
            iter=(1 + int(timeout / self._poll_frequency)),
        )

    def get_job_count(self, state, from_orchid=True, verbose=False):
        if from_orchid:
            base_path = self.get_path() + "/controller_orchid/progress/jobs/"
        else:
            base_path = self.get_path() + "/@progress/jobs/"

        try:
            path = base_path + str(state)
            if state == "aborted" or state == "completed":
                path += "/total"
            return get(path, verbose=verbose)
        except YtError as err:
            if not err.is_resolve_error():
                raise
            return 0

    def get_running_jobs(self, verbose=False):
        jobs_path = self.get_path() + "/controller_orchid/running_jobs"
        return get(jobs_path, verbose=verbose, default={})

    def get_runtime_state(self, **kwargs):
        return get("//sys/scheduler/orchid/scheduler/operations/{}/state".format(self.id), **kwargs)

    def get_runtime_progress(self, path=None, default=None):
        progress = get("//sys/scheduler/orchid/scheduler/operations/{}/progress".format(self.id))
        if path is None:
            return progress
        else:
            return get_at(progress, path, default)

    def get_state(self, **kwargs):
        try:
            return get(self.get_path() + "/@state", verbose_error=False, **kwargs)
        except YtResponseError as err:
            if not err.is_resolve_error():
                raise

        return get(self.get_path() + "/@state", **kwargs)

    def wait_for_state(self, state, **kwargs):
        wait(lambda: self.get_state(**kwargs) == state)

    def wait_for_fresh_snapshot(self):
        # It is important to repeat this procedure twice as there may already be an ongoing
        # snapshotting procedure which saved state arbitrarily long before the moment of calling.
        for _ in xrange(2):
            timepoint = datetime.utcnow()
            snapshot_path = self.get_path() + "/snapshot"
            wait(
                lambda: exists(snapshot_path)
                and date_string_to_datetime(get(snapshot_path + "/@creation_time")) > timepoint
            )

    def wait_presence_in_scheduler(self):
        wait(lambda: exists("//sys/scheduler/orchid/scheduler/operations/" + self.id))

    def get_alerts(self):
        try:
            return get(self.get_path() + "/@alerts")
        except YtResponseError as err:
            if err.is_resolve_error():
                return {}
            raise

    def list_jobs(self, **kwargs):
        return [job_info["id"] for job_info in list_jobs(self.id, **kwargs)["jobs"]]

    def read_stderr(self, job_id):
        return get_job_stderr(self.id, job_id)

    def get_error(self):
        state = self.get_state(verbose=False)
        if state == "failed":
            error = get(self.get_path() + "/@result/error", verbose=False)
            job_ids = self.list_jobs(with_stderr=True)
            job_errors = []
            for job_id in job_ids:
                job_info = get_job(self.id, job_id)
                if job_info["state"] not in ("completed", "failed", "aborted"):
                    continue
                if not job_info.get("error"):
                    continue

                job_error = job_info["error"]
                message = job_info["error"]["message"]
                stderr = self.read_stderr(job_id)
                if stderr:
                    message += "\n" + stderr.decode("utf-8", errors="ignore")
                job_errors.append(
                    YtError(
                        message=message,
                        code=job_error.get("code", 1),
                        attributes=job_error.get("attributes"),
                        inner_errors=job_error.get("inner_errors"),
                    )
                )
            inner_errors = error.get("inner_errors", [])
            if len(job_errors) > 0:
                inner_errors.append(YtError(message="Some of the jobs have failed", inner_errors=job_errors))

            return YtError(
                message=error["message"],
                code=error.get("code"),
                attributes=error["attributes"],
                inner_errors=inner_errors,
            )
        if state == "aborted":
            return YtError(message="Operation {0} aborted".format(self.id))

    def build_progress(self):
        try:
            progress = get(
                self.get_path() + "/@brief_progress/jobs",
                verbose=False,
                verbose_error=False,
            )
        except YtError:
            return "(brief progress is not available yet)"

        result = {}
        for job_type in progress:
            if isinstance(progress[job_type], dict):
                result[job_type] = progress[job_type]["total"]
            else:
                result[job_type] = progress[job_type]
        return "({0})".format(", ".join("{0}={1}".format(key, result[key]) for key in result))

    def track(self, raise_on_failed=True, raise_on_aborted=True, timeout=timedelta(minutes=10)):
        now = datetime.now()
        deadline = now + timeout
        counter = 0
        operation_finished = False
        while now < deadline:
            state = self.get_state(verbose=False)
            message = "Operation {0} {1}".format(self.id, state)
            if counter % 30 == 0 or state in ["failed", "aborted", "completed"]:
                print_debug(message)
                if state == "running":
                    print_debug(self.build_progress())
                    print_debug()
                else:
                    print_debug()
            if state == "failed" and raise_on_failed:
                raise self.get_error()
            if state == "aborted" and raise_on_aborted:
                raise self.get_error()
            if state in ["failed", "aborted", "completed"]:
                operation_finished = True
                break
            counter += 1
            time.sleep(self._poll_frequency)
            now = datetime.now()

        if not operation_finished:
            raise YtError("Operation {} has not finished in {} seconds".format(self.id, timeout.total_seconds()))

    def abort(self, wait_until_finished=False, **kwargs):
        abort_op(self.id, **kwargs)
        if wait_until_finished:
            self.wait_for_state("aborted")

    def complete(self, wait_until_finished=False, **kwargs):
        complete_op(self.id, **kwargs)
        if wait_until_finished:
            self.wait_for_state("completed")

    def suspend(self, **kwargs):
        suspend_op(self.id, **kwargs)

    def resume(self, **kwargs):
        resume_op(self.id, **kwargs)

    def get_orchid_path(self):
        path = self.get_path() + "/@controller_agent_address"
        wait(lambda: exists(path))
        controller_agent = get(path)
        return "//sys/controller_agents/instances/{}/orchid/controller_agent/operations/{}".format(
            controller_agent, self.id
        )

    def lookup_in_archive(self):
        id_hi, id_lo = uuid_to_parts(self.id)
        rows = lookup_rows("//sys/operations_archive/ordered_by_id", [{"id_hi": id_hi, "id_lo": id_lo}])
        assert len(rows) == 1
        return rows[0]


def create_tmpdir(prefix):
    basedir = os.path.join(path_to_run_tests, "tmp")
    try:
        if not os.path.exists(basedir):
            os.mkdir(basedir)
    except OSError:
        sys.excepthook(*sys.exc_info())

    tmpdir = tempfile.mkdtemp(prefix="{0}_{1}_".format(prefix, os.getpid()), dir=basedir)
    # Give full access to tmpdir, it must be accessible from user jobs
    # to implement waitable jobs.
    os.chmod(tmpdir, stat.S_IRWXU | stat.S_IRWXG | stat.S_IRWXO)
    return tmpdir


def start_op(op_type, **kwargs):
    op_name = None
    if op_type == "map":
        op_name = "mapper"
    if op_type == "reduce" or op_type == "join_reduce":
        op_name = "reducer"

    input_name = None
    if op_type != "erase" and op_type != "vanilla":
        kwargs["in_"] = prepare_paths(kwargs["in_"])
        input_name = "input_table_paths"

    output_name = None
    if "out" in kwargs:
        if op_type in ["map", "reduce", "join_reduce", "map_reduce"]:
            kwargs["out"] = prepare_paths(kwargs["out"])
            output_name = "output_table_paths"
        else:
            kwargs["out"] = prepare_path(kwargs["out"])
            output_name = "output_table_path"

    if "file" in kwargs:
        kwargs["file"] = prepare_paths(kwargs["file"])

    for opt in ["sort_by", "reduce_by", "join_by"]:
        flat(kwargs, opt)

    operation = Operation()

    change(kwargs, "table_path", ["spec", "table_path"])
    change(kwargs, "in_", ["spec", input_name])
    change(kwargs, "out", ["spec", output_name])
    change(kwargs, "command", ["spec", op_name, "command"])
    change(kwargs, "file", ["spec", op_name, "file_paths"])
    change(kwargs, "sort_by", ["spec", "sort_by"])
    change(kwargs, "reduce_by", ["spec", "reduce_by"])
    change(kwargs, "join_by", ["spec", "join_by"])
    change(kwargs, "mapper_file", ["spec", "mapper", "file_paths"])
    change(kwargs, "reduce_combiner_file", ["spec", "reduce_combiner", "file_paths"])
    change(kwargs, "reducer_file", ["spec", "reducer", "file_paths"])
    change(kwargs, "mapper_command", ["spec", "mapper", "command"])
    change(kwargs, "reduce_combiner_command", ["spec", "reduce_combiner", "command"])
    change(kwargs, "reducer_command", ["spec", "reducer", "command"])

    track = kwargs.get("track", True)
    if "track" in kwargs:
        del kwargs["track"]

    kwargs["operation_type"] = op_type

    if kwargs.get("return_response", False):
        return execute_command("start_op", kwargs, parse_yson=True, return_response=True)

    operation.id = execute_command("start_op", kwargs, parse_yson=True)
    if track:
        operation.track()
    return operation


def operation_nodes_exist():
    for entry in ls("//sys/operations"):
        if len(entry) != 2:
            return True
        if ls("//sys/operations/" + entry):
            return True
    return False


def clean_operations():
    cleaner_path = "//sys/scheduler/config/operations_cleaner"
    try:
        set(cleaner_path + "/enable", True, recursive=True)
        wait(lambda: not operation_nodes_exist())
    finally:
        set(cleaner_path + "/enable", False)


def resolve_operation_id_or_alias(command):
    def resolved_command(op_id_or_alias, **kwargs):
        if op_id_or_alias.startswith("*"):
            kwargs["operation_alias"] = op_id_or_alias
        else:
            kwargs["operation_id"] = op_id_or_alias
        return command(**kwargs)

    return resolved_command


@resolve_operation_id_or_alias
def abort_op(**kwargs):
    execute_command("abort_op", kwargs)


@resolve_operation_id_or_alias
def suspend_op(**kwargs):
    execute_command("suspend_op", kwargs)


@resolve_operation_id_or_alias
def complete_op(**kwargs):
    execute_command("complete_op", kwargs)


@resolve_operation_id_or_alias
def resume_op(**kwargs):
    execute_command("resume_op", kwargs)


@resolve_operation_id_or_alias
def get_operation(is_raw=False, **kwargs):
    result = execute_command("get_operation", kwargs)
    return result if is_raw else yson.loads(result)


@resolve_operation_id_or_alias
def update_op_parameters(**kwargs):
    execute_command("update_op_parameters", kwargs)


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


def vanilla(**kwargs):
    return start_op("vanilla", **kwargs)


def erase(path, **kwargs):
    kwargs["table_path"] = path
    change(kwargs, "combine_chunks", ["spec", "combine_chunks"])
    return start_op("erase", **kwargs)


def sort(**kwargs):
    return start_op("sort", **kwargs)


def remote_copy(**kwargs):
    return start_op("remote_copy", **kwargs)


def build_snapshot(*args, **kwargs):
    return get_driver().build_snapshot(*args, **kwargs)


def build_master_snapshots(*args, **kwargs):
    return get_driver().build_master_snapshots(*args, **kwargs)


def switch_leader(cell_id, new_leader_address):
    parameters = {"cell_id": cell_id, "new_leader_address": new_leader_address}
    return execute_command("switch_leader", parameters, parse_yson=True)


def repair_exec_node(address, locations):
    parameters = {"address": address, "locations": locations}
    return execute_command("repair_exec_node", parameters, parse_yson=True)


def set_node_resource_targets(address, cpu_limit, memory_limit):
    _get_driver(driver=None).set_node_resource_targets(
        address=address,
        cpu_limit=cpu_limit,
        memory_limit=memory_limit)


def get_version():
    return execute_command("get_version", {}, parse_yson=True)


def gc_collect(driver=None):
    _get_driver(driver=driver).gc_collect()


def clear_metadata_caches(driver=None):
    _get_driver(driver=driver).clear_metadata_caches()


def create_account(name, parent_name=None, empty=False, **kwargs):
    sync = kwargs.pop("sync_creation", True)
    kwargs["type"] = "account"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    if parent_name is not None:
        kwargs["attributes"]["parent_name"] = parent_name
    if not empty and "resource_limits" not in kwargs["attributes"]:
        kwargs["attributes"]["resource_limits"] = {
            "disk_space_per_medium": {"default": 2 ** 30},
            "chunk_count": 100000,
            "node_count": 1000,
            "tablet_count": 0,
            "tablet_static_memory": 0,
        }

    set_master_memory = True
    if "resource_limits" in kwargs["attributes"]:
        resource_limits = kwargs["attributes"]["resource_limits"]
        if isinstance(resource_limits, dict) and "master_memory" in resource_limits:
            set_master_memory = resource_limits["master_memory"].get("total", 0) == 0

    driver = kwargs.get("driver")

    execute_command("create", kwargs)
    driver = _get_driver(kwargs.get("driver", None))
    if sync:
        wait(
            lambda: exists("//sys/accounts/{0}".format(name), driver=driver)
            and get("//sys/accounts/{0}/@life_stage".format(name), driver=driver) == "creation_committed"
        )
    if set_master_memory:
        try:
            set(
                "//sys/accounts/{0}/@resource_limits/master_memory/total".format(name),
                100000,
                driver=driver,
            )
            set(
                "//sys/accounts/{0}/@resource_limits/master_memory/chunk_host".format(name),
                100000,
                driver=driver,
            )
        except YtError:
            set(
                "//sys/accounts/{0}/@resource_limits/master_memory".format(name),
                100000,
                driver=driver,
            )


def remove_account(name, **kwargs):
    driver = kwargs.get("driver")
    gc_collect(driver)
    sync = kwargs.pop("sync_deletion", True)
    account_path = "//sys/accounts/" + name
    remove(account_path, **kwargs)
    if sync:
        wait(lambda: not exists(account_path, driver=driver))


def create_account_resource_usage_lease(account, transaction_id, **kwargs):
    kwargs["type"] = "account_resource_usage_lease"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["account"] = account
    kwargs["attributes"]["transaction_id"] = transaction_id
    return execute_command("create", kwargs, parse_yson=True)


def create_pool_tree(name, config=None, wait_for_orchid=True, allow_patching=True, **kwargs):
    kwargs["type"] = "scheduler_pool_tree"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    if config is None:
        config = {}
    if allow_patching:
        if "min_child_heap_size" not in config:
            config["min_child_heap_size"] = 3

    kwargs["attributes"]["config"] = update(kwargs["attributes"].get("config", {}), config)

    execute_command("create", kwargs, parse_yson=True)
    if wait_for_orchid:
        wait(lambda: exists(yt_scheduler_helpers.scheduler_orchid_pool_tree_path(name)))


def remove_pool_tree(name, wait_for_orchid=True, **kwargs):
    remove("//sys/pool_trees/" + name, **kwargs)
    if wait_for_orchid:
        wait(lambda: not exists(yt_scheduler_helpers.scheduler_orchid_pool_tree_path(name)))


def create_pool(name, pool_tree="default", parent_name=None, wait_for_orchid=True, **kwargs):
    kwargs["type"] = "scheduler_pool"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    kwargs["attributes"]["pool_tree"] = pool_tree
    if parent_name:
        kwargs["attributes"]["parent_name"] = parent_name
    execute_command("create", kwargs, parse_yson=True)
    if wait_for_orchid:
        wait(lambda: exists(yt_scheduler_helpers.scheduler_orchid_pool_path(name, pool_tree)))
        expected_parent_name = parent_name if parent_name else "<Root>"
        wait(lambda: get(yt_scheduler_helpers.scheduler_orchid_pool_path(name, pool_tree))["parent"] == expected_parent_name)


def create_user(name, **kwargs):
    kwargs["type"] = "user"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    driver = kwargs.get("driver")
    result = execute_command("create", kwargs)
    wait(
        lambda: exists("//sys/users/{0}".format(name), driver=driver)
        and get("//sys/users/{0}/@life_stage".format(name), driver=driver) == "creation_committed"
    )
    return result


def remove_user(name, **kwargs):
    driver = kwargs.get("driver")
    gc_collect(driver)
    sync = kwargs.pop("sync_deletion", True)
    user_path = "//sys/users/" + name
    remove(user_path, **kwargs)
    if sync:
        wait(lambda: not exists(user_path, driver=driver))


def make_random_string(length=10):
    return "".join(random.choice(ascii_letters) for _ in xrange(length))


def create_test_tables(row_count=1, **kwargs):
    create("table", "//tmp/t_in", **kwargs)
    write_table("//tmp/t_in", [{"x": str(i)} for i in xrange(row_count)])
    create("table", "//tmp/t_out", **kwargs)


def run_test_vanilla(command, spec=None, job_count=1, track=False, task_patch=None, **kwargs):
    spec = spec or {}
    spec["tasks"] = {"task": update({"job_count": job_count, "command": command}, task_patch)}
    return vanilla(spec=spec, track=track, **kwargs)


def run_sleeping_vanilla(**kwargs):
    return run_test_vanilla("sleep 1000", **kwargs)


def enable_op_detailed_logs(op):
    update_op_parameters(
        op.id,
        parameters={
            "scheduling_options_per_pool_tree": {
                "default": {
                    "enable_detailed_logs": True,
                }
            }
        },
    )


def create_group(name, **kwargs):
    kwargs["type"] = "group"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    return execute_command("create", kwargs)


def remove_group(name, **kwargs):
    remove("//sys/groups/" + name, **kwargs)


def add_member(member, group, **kwargs):
    kwargs["member"] = member
    kwargs["group"] = group
    execute_command("add_member", kwargs)


def remove_member(member, group, **kwargs):
    kwargs["member"] = member
    kwargs["group"] = group
    execute_command("remove_member", kwargs)


def create_network_project(name, **kwargs):
    kwargs["type"] = "network_project"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)


def remove_network_project(name, **kwargs):
    remove("//sys/network_projects/" + name, **kwargs)


def create_proxy_role(name, proxy_kind, **kwargs):
    kwargs["type"] = "proxy_role"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    kwargs["attributes"]["proxy_kind"] = proxy_kind
    execute_command("create", kwargs)


def remove_proxy_role(name, proxy_kind, **kwargs):
    remove("//sys/{}_proxy_roles/{}".format(proxy_kind, name), **kwargs)


def create_tablet_cell(**kwargs):
    kwargs["type"] = "tablet_cell"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    return execute_command("create", kwargs, parse_yson=True)


def create_tablet_cell_bundle(name, initialize_options=True, **kwargs):
    kwargs["type"] = "tablet_cell_bundle"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    if initialize_options:
        if "options" not in kwargs["attributes"]:
            kwargs["attributes"]["options"] = dict()
        for option in ("changelog_account", "snapshot_account"):
            if option not in kwargs["attributes"]["options"]:
                kwargs["attributes"]["options"][option] = "sys"
    driver = _get_driver(kwargs.get("driver", None))
    execute_command("create", kwargs)
    wait(
        lambda: exists("//sys/tablet_cell_bundles/{0}".format(name), driver=driver)
        and get("//sys/tablet_cell_bundles/{0}/@life_stage".format(name), driver=driver) == "creation_committed"
    )


def remove_tablet_cell_bundle(name, driver=None):
    areas = get("//sys/tablet_cell_bundles/{0}/@areas".format(name), driver=driver)
    for area in areas.values():
        remove("#{0}".format(area["id"]), driver=driver)
    remove("//sys/tablet_cell_bundles/" + name, driver=driver)


def remove_tablet_cell(id, driver=None):
    remove("//sys/tablet_cells/" + id, driver=driver, force=True)


def sync_remove_tablet_cells(cells, driver=None):
    cells = builtins.set(cells)
    for id in cells:
        remove_tablet_cell(id, driver=driver)
    wait(lambda: len(cells.intersection(builtins.set(get("//sys/tablet_cells", driver=driver)))) == 0)


def remove_tablet_action(id, driver=None):
    remove("#" + id, driver=driver)


def create_table_replica(table_path, cluster_name, replica_path, **kwargs):
    kwargs["type"] = "table_replica"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["table_path"] = table_path
    kwargs["attributes"]["cluster_name"] = cluster_name
    kwargs["attributes"]["replica_path"] = replica_path
    return execute_command("create", kwargs, parse_yson=True)


def remove_table_replica(replica_id):
    remove("#{0}".format(replica_id))


def alter_table_replica(replica_id, **kwargs):
    kwargs["replica_id"] = replica_id
    execute_command("alter_table_replica", kwargs)


def create_table_collocation(table_ids=None, table_paths=None, **kwargs):
    kwargs["type"] = "table_collocation"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["collocation_type"] = "replication"
    if table_ids is not None:
        kwargs["attributes"]["table_ids"] = table_ids
    if table_paths is not None:
        kwargs["attributes"]["table_paths"] = table_paths
    return execute_command("create", kwargs, parse_yson=True)


def create_data_center(name, **kwargs):
    kwargs["type"] = "data_center"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)


def remove_data_center(name, **kwargs):
    remove("//sys/data_centers/" + name, **kwargs)


def create_rack(name, **kwargs):
    kwargs["type"] = "rack"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)


def remove_rack(name, **kwargs):
    remove("//sys/racks/" + name, **kwargs)


def create_host(name, **kwargs):
    kwargs["type"] = "host"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)


def remove_host(name, **kwargs):
    remove("//sys/hosts/" + name, **kwargs)


def create_medium(name, **kwargs):
    kwargs["type"] = "medium"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    execute_command("create", kwargs)


def create_replication_card(**kwargs):
    return execute_command("create_replication_card", kwargs, parse_yson=True)


def get_replication_card(**kwargs):
    return execute_command("get_replication_card", kwargs, parse_yson=True)


def create_replication_card_replica(**kwargs):
    return execute_command("create_replication_card_replica", kwargs, parse_yson=True)


def remove_replication_card_replica(**kwargs):
    return execute_command("remove_replication_card_replica", kwargs, parse_yson=True)


def alter_replication_card_replica(**kwargs):
    return execute_command("alter_replication_card_replica", kwargs, parse_yson=True)

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
    return ls("//sys/accounts", driver=driver)


def get_users(driver=None):
    return ls("//sys/users", driver=driver)


def get_groups(driver=None):
    return ls("//sys/groups", driver=driver)


def get_tablet_cells(driver=None):
    return ls("//sys/tablet_cells", driver=driver)


def get_tablet_actions(driver=None):
    return ls("//sys/tablet_actions", driver=driver)


def get_data_centers(driver=None):
    return ls("//sys/data_centers", driver=driver)


def get_racks(driver=None):
    return ls("//sys/racks", driver=driver)


def get_nodes(driver=None):
    return ls("//sys/cluster_nodes", driver=driver)


def get_media(driver=None):
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


def get_account_disk_space_limit(account, medium="default"):
    disk_space = get("//sys/accounts/{0}/@resource_limits/disk_space_per_medium".format(account))
    return disk_space.get(medium, 0)


def set_account_disk_space_limit(account, limit, medium="default"):
    set(
        "//sys/accounts/{0}/@resource_limits/disk_space_per_medium/{1}".format(account, medium),
        limit,
    )


# NB: does not check master_memory yet!
def cluster_resources_equal(a, b):
    if (a.get("disk_space", 0) != b.get("disk_space", 0)
            or a.get("chunk_count", 0) != b.get("chunk_count", 0)
            or a.get("node_count", 0) != b.get("node_count", 0)
            or a.get("tablet_count", 0) != b.get("tablet_count", 0)
            or a.get("tablet_static_memory", 0) != b.get("tablet_static_memory", 0)):
        return False

    media = builtins.set(iterkeys(a.get("disk_space_per_medium", {})))
    media.union(builtins.set(iterkeys(b.get("disk_space_per_medium", {}))))
    return all(
        a.get("disk_space_per_medium", {}).get(medium, 0) == b.get("disk_space_per_medium", {}).get(medium, 0)
        for medium in media
    )


def get_chunk_replication_factor(chunk_id):
    return get("#{0}/@media/default/replication_factor".format(chunk_id))


def make_ace(
    action,
    subjects,
    permissions,
    inheritance_mode="object_and_descendants",
    columns=None,
):
    def _to_list(x):
        if isinstance(x, str):
            return [x]
        else:
            return x

    ace = {
        "action": action,
        "subjects": _to_list(subjects),
        "permissions": _to_list(permissions),
        "inheritance_mode": inheritance_mode,
    }
    if columns is not None:
        ace["columns"] = _to_list(columns)
    return ace

##################################################################


def get_guid_from_parts(lo, hi):
    assert 0 <= lo < 2 ** 64
    assert 0 <= hi < 2 ** 64
    ints = [0, 0, 0, 0]
    ints[0] = hi & 0xFFFFFFFF
    ints[1] = hi >> 32
    ints[2] = lo & 0xFFFFFFFF
    ints[3] = lo >> 32
    return "{3:x}-{2:x}-{1:x}-{0:x}".format(*ints)


def generate_uuid(generator=None):
    def get_int():
        return hex(random.randint(0, 2 ** 32 - 1))[2:].rstrip("L")

    return "-".join([get_int() for _ in xrange(4)])


##################################################################


def get_statistics(statistics, complex_key):
    result = statistics
    for part in complex_key.split("."):
        if part:
            if part not in result:
                return None
            result = result[part]
    return result


##################################################################


def check_all_stderrs(op, expected_content, expected_count, substring=False):
    job_infos = list_jobs(op.id, with_stderr=True)["jobs"]
    job_with_stderr_count = 0
    for job_info in job_infos:
        if job_info["state"] == "running":
            continue

        job_with_stderr_count += 1
        job_id = job_info["id"]
        content = op.read_stderr(job_id)
        if substring:
            assert expected_content in content
        else:
            assert content == expected_content
    assert job_with_stderr_count == expected_count

##################################################################


def set_banned_flag(value, nodes=None, driver=None):
    if value:
        flag = True
        expected_state = "offline"
    else:
        flag = False
        expected_state = "online"

    if not nodes:
        nodes = ls("//sys/cluster_nodes", driver=driver)

    for address in nodes:
        set("//sys/cluster_nodes/{0}/@banned".format(address), flag, driver=driver)

    def check():
        return all(
            get("//sys/cluster_nodes/{0}/@state".format(address), driver=driver) == expected_state for address in nodes
        )

    wait(check)


##################################################################


class PrepareTables(object):
    def _create_table(self, table):
        create("table", table)
        set(table + "/@replication_factor", 1)

    def _prepare_tables(self):
        self._create_table("//tmp/t_in")
        write_table("//tmp/t_in", {"foo": "bar"})

        self._create_table("//tmp/t_out")


##################################################################


def set_node_banned(address, flag, driver=None):
    set("//sys/cluster_nodes/%s/@banned" % address, flag, driver=driver)
    ban, state = ("banned", "offline") if flag else ("unbanned", "online")
    print_debug("Waiting for node %s to become %s..." % (address, ban))
    wait(lambda: get("//sys/cluster_nodes/%s/@state" % address, driver=driver) == state)


def set_node_decommissioned(address, flag, driver=None):
    set("//sys/cluster_nodes/%s/@decommissioned" % address, flag, driver=driver)
    print_debug("Node %s is %s" % (address, "decommissioned" if flag else "not decommissioned"))


def wait_for_nodes(driver=None):
    print_debug("Waiting for nodes to become online...")
    wait(
        lambda: all(
            n.attributes["state"] == "online" for n in ls("//sys/cluster_nodes", attributes=["state"], driver=driver)
        )
    )


def wait_for_chunk_replicator(driver=None):
    print_debug("Waiting for chunk replicator to become enabled...")
    wait(lambda: get("//sys/@chunk_replicator_enabled", driver=driver))


def get_cluster_drivers(primary_driver=None):
    if primary_driver is None:
        return [drivers_by_cell_tag[default_api_version] for drivers_by_cell_tag in clusters_drivers["primary"]]
    for drivers in clusters_drivers.values():
        if drivers[0][default_api_version] == primary_driver:
            return [drivers_by_cell_tag[default_api_version] for drivers_by_cell_tag in drivers]
    raise "Failed to get cluster drivers"


def wait_for_cells(cell_ids=None, decommissioned_addresses=[], driver=None):
    print_debug("Waiting for tablet cells to become healthy...")

    def get_cells(driver):
        cells = ls(
            "//sys/tablet_cells",
            attributes=["health", "id", "peers", "config_version"],
            driver=driver,
        )
        if cell_ids is None:
            return cells
        return [cell for cell in cells if cell.attributes["id"] in cell_ids]

    def check_orchid():
        cells = get_cells(driver=driver)
        for cell in cells:
            cell_id = str(cell)
            expected_config_version = cell.attributes["config_version"]
            peers = cell.attributes["peers"]
            for peer in peers:
                address = peer.get("address", None)
                if address is None or address in decommissioned_addresses:
                    return False
                try:
                    actual_config_version = get(
                        "//sys/cluster_nodes/{0}/orchid/tablet_cells/{1}/config_version".format(address, cell_id),
                        driver=driver,
                    )
                    if actual_config_version != expected_config_version:
                        return False

                    active = get(
                        "//sys/cluster_nodes/{0}/orchid/tablet_cells/{1}/hydra/active".format(address, cell_id),
                        driver=driver,
                    )
                    if not active:
                        return False
                except YtError:
                    return False
        return True

    wait(check_orchid)

    def check_cells(driver):
        cells = get_cells(driver=driver)
        for cell in cells:
            if cell.attributes["health"] != "good":
                return False
        return True

    for driver in get_cluster_drivers(driver):
        wait(lambda: check_cells(driver=driver))


def sync_create_cells(cell_count, driver=None, **attributes):
    cell_ids = []
    for _ in xrange(cell_count):
        cell_id = create_tablet_cell(attributes=attributes, driver=driver)
        cell_ids.append(cell_id)
    wait_for_cells(cell_ids, driver=driver)
    return cell_ids


def wait_until_sealed(path, driver=None):
    wait(lambda: get(path + "/@sealed", driver=driver))


def truncate_journal(path, row_count, **kwargs):
    kwargs["path"] = path
    kwargs["row_count"] = row_count
    return execute_command("truncate_journal", kwargs)


def wait_for_tablet_state(path, state, **kwargs):
    print_debug("Waiting for tablets to become %s..." % (state))
    driver = kwargs.pop("driver", None)
    if kwargs.get("first_tablet_index", None) is None and kwargs.get("last_tablet_index", None) is None:
        wait(lambda: get(path + "/@tablet_state", driver=driver) == state)
    else:
        tablet_count = get(path + "/@tablet_count", driver=driver)
        first_tablet_index = kwargs.get("first_tablet_index", 0)
        last_tablet_index = kwargs.get("last_tablet_index", tablet_count - 1)
        wait(
            lambda: all(
                x["state"] == state
                for x in get(path + "/@tablets", driver=driver)[first_tablet_index:last_tablet_index + 1]
            )
        )
        wait(lambda: get(path + "/@tablet_state") != "transient")


def sync_mount_table(path, freeze=False, **kwargs):
    mount_table(path, freeze=freeze, **kwargs)
    wait_for_tablet_state(path, "frozen" if freeze else "mounted", **kwargs)


def sync_unmount_table(path, **kwargs):
    unmount_table(path, **kwargs)
    wait_for_tablet_state(path, "unmounted", **kwargs)


def sync_freeze_table(path, **kwargs):
    freeze_table(path, **kwargs)
    wait_for_tablet_state(path, "frozen", **kwargs)


def sync_unfreeze_table(path, **kwargs):
    unfreeze_table(path, **kwargs)
    wait_for_tablet_state(path, "mounted", **kwargs)


def sync_reshard_table(path, *args, **kwargs):
    reshard_table(path, *args, **kwargs)
    driver = kwargs.get("driver", None)
    wait(lambda: get(path + "/@tablet_state", driver=driver) != "transient")


def _wait_for_tablet_actions(action_ids):
    wait(
        lambda: all(
            get("//sys/tablet_actions/{}/@state".format(action_id)) in ("completed", "failed")
            for action_id in action_ids
        )
    )


def sync_reshard_table_automatic(path, **kwargs):
    kwargs["keep_actions"] = True
    rsp = reshard_table_automatic(path, **kwargs)
    _wait_for_tablet_actions(rsp)


def sync_balance_tablet_cells(bundle, tables=None, **kwargs):
    kwargs["keep_actions"] = True
    rsp = balance_tablet_cells(bundle, tables, **kwargs)
    _wait_for_tablet_actions(rsp)


def sync_flush_table(path, driver=None):
    sync_freeze_table(path, driver=driver)
    sync_unfreeze_table(path, driver=driver)


def sync_compact_table(path, driver=None):
    chunk_ids = builtins.set(get(path + "/@chunk_ids", driver=driver))
    sync_unmount_table(path, driver=driver)
    set(path + "/@forced_compaction_revision", 1, driver=driver)
    sync_mount_table(path, driver=driver)

    print_debug("Waiting for tablets to become compacted...")
    wait(lambda: len(chunk_ids.intersection(builtins.set(get(path + "/@chunk_ids", driver=driver)))) == 0)

    print_debug("Waiting for tablets to become stable...")

    def check_stable():
        chunk_ids1 = get(path + "/@chunk_ids", driver=driver)
        time.sleep(3.0)
        chunk_ids2 = get(path + "/@chunk_ids", driver=driver)
        return chunk_ids1 == chunk_ids2

    wait(lambda: check_stable())


def sync_enable_table_replica(replica_id, driver=None):
    alter_table_replica(replica_id, enabled=True, driver=driver)

    print_debug("Waiting for replica to become enabled...")
    wait(lambda: get("#{0}/@state".format(replica_id), driver=driver) == "enabled")


def sync_disable_table_replica(replica_id, driver=None):
    alter_table_replica(replica_id, enabled=False, driver=driver)

    print_debug("Waiting for replica to become disabled...")
    wait(lambda: get("#{0}/@state".format(replica_id), driver=driver) == "disabled")


def get_tablet_leader_address(tablet_id):
    cell_id = get("//sys/tablets/" + tablet_id + "/@cell_id")
    peers = get("//sys/tablet_cells/" + cell_id + "/@peers")
    leader_peer = list(x for x in peers if x["state"] == "leading")[0]
    return leader_peer["address"]


def sync_alter_table_replica_mode(replica_id, mode, driver=None):
    alter_table_replica(replica_id, mode=mode, driver=driver)

    print_debug("Waiting for replica mode to become {}...".format(mode))
    tablets = get("#{}/@tablets".format(replica_id), driver=driver)
    tablet_ids = [x["tablet_id"] for x in tablets]
    cell_ids = [get("#{}/@cell_id".format(tablet_id), driver=driver) for tablet_id in tablet_ids]
    addresses = [get_tablet_leader_address(tablet_id) for tablet_id in tablet_ids]

    def check():
        for tablet_id, cell_id, address in zip(tablet_ids, cell_ids, addresses):
            actual_mode = get(
                "//sys/cluster_nodes/{}/orchid/tablet_cells/{}/tablets/{}/replicas/{}/mode".format(
                    address, cell_id, tablet_id, replica_id
                ),
                driver=driver,
            )
            if actual_mode != mode:
                return False
        return True

    wait(check)


def create_table(path, force=None, dynamic=None, schema=None):
    kwargs = {}
    if force is not None:
        kwargs["force"] = force
    if dynamic is not None:
        kwargs.setdefault("attributes", {})["dynamic"] = dynamic
    if schema is not None:
        kwargs.setdefault("attributes", {})["schema"] = schema
    create("table", path, **kwargs)


def create_dynamic_table(path, schema=None, driver=None, **attributes):
    if "dynamic" not in attributes:
        attributes.update({"dynamic": True})

    if "enable_dynamic_store_read" not in attributes:
        attributes.update({"enable_dynamic_store_read": True})
    elif attributes["enable_dynamic_store_read"] is None:
        del attributes["enable_dynamic_store_read"]

    attributes.update({"schema": schema})
    return create("table", path, attributes=attributes, driver=driver)


def create_area(name, cell_bundle_id, **kwargs):
    kwargs["type"] = "area"
    if "attributes" not in kwargs:
        kwargs["attributes"] = dict()
    kwargs["attributes"]["name"] = name
    kwargs["attributes"]["cell_bundle_id"] = cell_bundle_id
    return execute_command("create", kwargs, parse_yson=True)


def remove_area(id, **kwargs):
    remove("#" + id, **kwargs)


def sync_control_chunk_replicator(enabled):
    print_debug("Setting chunk replicator state to", enabled)
    set("//sys/@config/chunk_manager/enable_chunk_replicator", enabled, recursive=True)
    wait(
        lambda: all(
            get(
                "//sys/@chunk_replicator_enabled",
                driver=drivers_by_cell_tag[default_api_version],
            ) == enabled
            for drivers_by_cell_tag in clusters_drivers["primary"]
        )
    )


def get_singular_chunk_id(path, **kwargs):
    chunk_ids = get(path + "/@chunk_ids", **kwargs)
    assert len(chunk_ids) == 1
    return chunk_ids[0]


def get_first_chunk_id(path, **kwargs):
    return get(path + "/@chunk_ids/0", **kwargs)


def get_applied_node_dynamic_config(node):
    return get("//sys/cluster_nodes/{0}/orchid/dynamic_config_manager/applied_config".format(node))


# Implements config.update(new_config) for dynamic nodes config and waits for config apply
# assuming the only nodes config filter is `%true`.
def update_nodes_dynamic_config(new_config):
    current_config = get("//sys/cluster_nodes/@config")
    current_config["%true"].update(new_config)
    set("//sys/cluster_nodes/@config", current_config)

    for node in ls("//sys/cluster_nodes"):
        wait(lambda: get_applied_node_dynamic_config(node) == current_config["%true"])


def update_controller_agent_config(path, value, wait_for_update=True):
    set("//sys/controller_agents/config/" + path, value, recursive=True)
    if wait_for_update:
        for agent in ls("//sys/controller_agents/instances"):
            orchid_config_path = "//sys/controller_agents/instances/{}/orchid/controller_agent/config".format(agent)
            wait(lambda: get("{}/{}".format(orchid_config_path, path)) == value)


def update_scheduler_config(path, value):
    set("//sys/scheduler/config/" + path, value, recursive=True)
    orchid_path = "{}/{}".format("//sys/scheduler/orchid/scheduler/config", path)
    wait(lambda: exists(orchid_path))
    wait(lambda: get(orchid_path) == value)


def update_pool_tree_config_option(pool_tree, option, value):
    set("//sys/pool_trees/{}/@config/{}".format(pool_tree, option), value)
    wait(lambda: get("//sys/scheduler/orchid/scheduler/scheduling_info_per_pool_tree/{}/config/{}".format(pool_tree, option)) == value)


def update_pool_tree_config(pool_tree, config):
    for option, value in list(iteritems(config)):
        update_pool_tree_config_option(pool_tree, option, value)


def get_nodes_with_flavor(flavor):
    cluster_nodes = ls("//sys/cluster_nodes", attributes=["flavors"])
    nodes = []
    for node in cluster_nodes:
        # COMPAT(gritukan)
        if "flavors" not in node.attributes:
            nodes.append(str(node))
            continue

        if flavor in node.attributes["flavors"]:
            nodes.append(str(node))
    return nodes


def get_data_nodes():
    return get_nodes_with_flavor("data")


def get_exec_nodes():
    return get_nodes_with_flavor("exec")


def get_tablet_nodes():
    return get_nodes_with_flavor("tablet")


def get_chaos_nodes():
    return get_nodes_with_flavor("chaos")


def is_active_primary_master_leader(rpc_address):
    try:
        return get("//sys/primary_masters/{}/orchid/monitoring/hydra/active_leader".format(rpc_address))
    except YtError:
        return False


def is_active_primary_master_follower(rpc_address):
    try:
        return get("//sys/primary_masters/{}/orchid/monitoring/hydra/active_follower".format(rpc_address))
    except YtError:
        return False


def get_active_primary_master_leader_address(env_setup):
    while True:
        for rpc_address in env_setup.Env.configs["master"][0]["primary_master"]["addresses"]:
            if is_active_primary_master_leader(rpc_address):
                return rpc_address


def get_active_primary_master_follower_address(env_setup):
    while True:
        for rpc_address in env_setup.Env.configs["master"][0]["primary_master"]["addresses"]:
            if is_active_primary_master_follower(rpc_address):
                return rpc_address
