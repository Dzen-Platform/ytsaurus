from .config import get_config
from .errors import YtError, YtOperationFailedError, YtTimeoutError, YtResponseError
from .driver import make_request
from .http_helpers import get_proxy_url, get_retriable_errors
from .exceptions_catcher import ExceptionCatcher
from .cypress_commands import exists, get, list
from .ypath import ypath_join
from .file_commands import read_file
from . import yson

import yt.logger as logger
from yt.common import format_error, date_string_to_datetime

from yt.packages.decorator import decorator
from yt.packages.six import iteritems
from yt.packages.six.moves import builtins, filter as ifilter

import logging
from datetime import datetime
from time import sleep, time
from cStringIO import StringIO

OPERATIONS_PATH = "//sys/operations"

class OperationState(object):
    """State of operation (simple wrapper for string name)."""
    def __init__(self, name):
        self.name = name

    def is_finished(self):
        return self.name in ["aborted", "completed", "failed"]

    def is_unsuccessfully_finished(self):
        return self.name in ["aborted", "failed"]

    def is_running(self):
        return self.name == "running"

    def __eq__(self, other):
        return self.name == str(other)

    def __ne__(self, other):
        return not (self == other)

    def __repr__(self):
        return self.name

    def __str__(self):
        return self.name


class TimeWatcher(object):
    """Class for proper sleeping in waiting operation."""
    def __init__(self, min_interval, max_interval, slowdown_coef, timeout=None):
        """
        Initialise time watcher.

        :param min_interval: minimal sleeping interval
        :param max_interval: maximal sleeping interval
        :param slowdown_coef: growth coefficient of sleeping interval
        :param timeout: maximal total interval of waiting. If ``timeout`` is ``None``, time watcher wait for eternally.
        """
        self.min_interval = min_interval
        self.max_interval = max_interval
        self.slowdown_coef = slowdown_coef
        self.total_time = 0.0
        self.timeout_time = (time() + timeout) if (timeout is not None) else None

    def _bound(self, interval):
        return min(max(interval, self.min_interval), self.max_interval)

    def _is_time_up(self, time):
        """Is passed time up?"""
        if not self.timeout_time:
            return False
        return time >= self.timeout_time

    def is_time_up(self):
        """Is time elapsed?"""
        return self._is_time_up(time())

    def wait(self):
        """Sleep proper time. If timeout occurred, wake up."""
        if self.is_time_up():
            return
        pause = self._bound(self.total_time * self.slowdown_coef)
        current_time = time()
        if self._is_time_up(current_time + pause):
            pause = self.timeout_time - current_time
        self.total_time += pause
        sleep(pause)


class OperationProgressFormatter(logging.Formatter):
    def __init__(self, format="%(asctime)-15s\t%(message)s", date_format=None, start_time=None):
        logging.Formatter.__init__(self, format, date_format)
        if start_time is None:
            self._start_time = datetime.now()
        else:
            self._start_time = start_time

    def formatTime(self, record, date_format=None):
        created = datetime.fromtimestamp(record.created)
        if date_format is not None:
            return created.strftime(date_format)
        else:
            def total_minutes(time):
                return time.seconds // 60 + 60 * 24 * time.days
            elapsed = total_minutes(datetime.now() - self._start_time)
            time = datetime.now()
            if time.microsecond > 0:
                time = time.isoformat(" ")[:-3]
            else:
                time = time.isoformat(" ")
            return "{0} ({1:2} min)".format(time, elapsed)

def get_operation_attributes(operation, client=None):
    """Returns dict with operation attributes.

    :param operation: (string) operation id.
    :return: (dict) operation description
    """
    operation_path = ypath_join(OPERATIONS_PATH, operation)
    return get(operation_path + "/@", client=client)

def get_operation_state(operation, client=None):
    """Return current state of operation.

    :param operation: (string) operation id.
    Raise `YtError` if operation doesn't exists
    """
    config = get_config(client)
    retry_count = config["proxy"]["request_retry_count"]
    config["proxy"]["request_retry_count"] = config["proxy"]["operation_state_discovery_retry_count"]
    try:
        return OperationState(get_operation_attributes(operation, client=client)["state"])
    finally:
        config["proxy"]["request_retry_count"] = retry_count

def get_operation_progress(operation, client=None):
    try:
        attributes = get_operation_attributes(operation, client=client)
        progress = attributes.get("progress", {}).get("jobs", {})
        # Fix aborted progress counter
        if progress and isinstance(progress["aborted"], dict):
            progress["aborted"] = progress["aborted"]["total"]
    except YtResponseError as err:
        if err.is_resolve_error():
            progress = {}
        else:
            raise
    return progress

def order_progress(progress):
    keys = ["running", "completed", "pending", "failed", "aborted", "lost", "total"]
    result = []
    for key in keys:
        if key in progress:
            result.append((key, progress[key]))
    for key, value in iteritems(progress):
        if key not in keys:
            result.append((key, value))
    return result

class PrintOperationInfo(object):
    """Cache operation state and print info by update"""
    def __init__(self, operation, client=None):
        self.operation = operation
        self.state = None
        self.progress = None

        creation_time_str = get_operation_attributes(operation, client=client)["creation_time"]
        creation_time = date_string_to_datetime(creation_time_str).replace(tzinfo=None)
        local_creation_time = creation_time + (datetime.now() - datetime.utcnow())

        self.formatter = OperationProgressFormatter(start_time=local_creation_time)

        self.client = client
        self.level = logging._levelNames[get_config(self.client)["operation_tracker"]["progress_logging_level"]]

    def __call__(self, state):
        if state.is_running():
            progress = get_operation_progress(self.operation, client=self.client)
            if progress != self.progress:
                self.log(
                    "operation %s: %s",
                    self.operation,
                    "\t".join("{0}={1}".format(k, v) for k, v in order_progress(progress)))
            self.progress = progress
        elif state != self.state:
            self.log("operation %s %s", self.operation, state)
        self.state = state

    def log(self, *args, **kwargs):
        if logger.LOGGER.isEnabledFor(self.level):
            logger.set_formatter(self.formatter)
            logger.log(self.level, *args, **kwargs)
            logger.set_formatter(logger.BASIC_FORMATTER)

def abort_operation(operation, client=None):
    """Abort operation.

    Do nothing if operation is in final state.

    :param operation: (string) operation id.
    """
    if get_operation_state(operation, client=client).is_finished():
        return
    make_request("abort_op", {"operation_id": operation}, client=client)

def suspend_operation(operation, client=None):
    """Suspend operation.

    :param operation: (string) operation id.
    """
    make_request("suspend_op", {"operation_id": operation}, client=client)

def resume_operation(operation, client=None):
    """Continue operation after suspending.

    :param operation: (string) operation id.
    """
    make_request("resume_op", {"operation_id": operation}, client=client)

def complete_operation(operation, client=None):
    """Complete operation.

    Abort all running and pending jobs.
    Preserve results of finished jobs.
    Do nothing if operation is in final state.

    :param operation: (string) operation id.
    """
    if get_operation_state(operation, client=client).is_finished():
        return
    make_request("complete_op", {"operation_id": operation}, client=client)

def get_operation_state_monitor(operation, time_watcher, action=lambda: None, client=None):
    """
    Yield state and sleep. Wait for final state of operation.

    If timeout occurred, abort operation and wait for final state anyway.

    :return: iterator over operation states.
    """
    while True:
        if time_watcher.is_time_up():
            abort_operation(operation, client=client)
            for state in get_operation_state_monitor(operation, TimeWatcher(1.0, 1.0, 0, timeout=None), client=client):
                yield state

        action()

        state = get_operation_state(operation, client=client)
        yield state
        if state.is_finished():
            break
        time_watcher.wait()


def get_stderrs(operation, only_failed_jobs, client=None):
    jobs_path = ypath_join(OPERATIONS_PATH, operation, "jobs")
    if not exists(jobs_path, client=client):
        return []
    jobs = list(jobs_path, attributes=["error", "address"], absolute=True, client=client)
    if only_failed_jobs:
        jobs = builtins.list(ifilter(lambda obj: "error" in obj.attributes, jobs))

    result = []

    for path in jobs:
        job_with_stderr = {}
        job_with_stderr["host"] = path.attributes["address"]

        if only_failed_jobs:
            job_with_stderr["error"] = path.attributes["error"]

        stderr_path = ypath_join(path, "stderr")
        has_stderr = exists(stderr_path, client=client)
        ignore_errors = get_config(client)["operation_tracker"]["ignore_stderr_if_download_failed"]
        if has_stderr:
            try:
                job_with_stderr["stderr"] = read_file(stderr_path, client=client).read()
            except tuple(builtins.list(get_retriable_errors()) + [YtResponseError]):
                if ignore_errors:
                    continue
                else:
                    raise

        if job_with_stderr:
            result.append(job_with_stderr)

    return result

def format_operation_stderrs(jobs_with_stderr):
    """
    Format operation jobs with stderr to string
    """

    output = StringIO()

    for job in jobs_with_stderr:
        output.write("Host: ")
        output.write(job["host"])
        output.write("\n")

        if "error" in job:
            output.write("Error:\n")
            output.write(format_error(job["error"]))
            output.write("\n")

        if "stderr" in job:
            output.write(job["stderr"])
            output.write("\n\n")

    return output.getvalue()

# TODO(ignat): is it convinient and generic way to get stderrs? Move to tests?
def add_failed_operation_stderrs_to_error_message(func):
    def _add_failed_operation_stderrs_to_error_message(func, *args, **kwargs):
        try:
            func(*args, **kwargs)
        except YtOperationFailedError as error:
            if "stderrs" in error.attributes:
                error.message = error.message + format_operation_stderrs(error.attributes["stderrs"])
            raise
    return decorator(_add_failed_operation_stderrs_to_error_message, func)

def get_operation_error(operation, client=None):
    # NB(ignat): conversion to json type necessary for json.dumps in TM.
    # TODO(ignat): we should decide what format should be used in errors (now it is yson both here and in http.py).
    result = yson.yson_to_json(get_operation_attributes(operation, client=client).get("result", {}))
    if "error" in result and result["error"]["code"] != 0:
        return result["error"]
    return None

def _create_operation_failed_error(operation, state):
    stderrs = get_stderrs(operation.id, only_failed_jobs=True, client=operation.client)
    error = get_operation_error(operation.id, client=operation.client)
    raise YtOperationFailedError(
        id=operation.id,
        state=str(state),
        error=error,
        stderrs=stderrs,
        url=operation.url)

class Operation(object):
    """Holds information about started operation."""
    def __init__(self, type, id, finalize=None, abort_exceptions=(KeyboardInterrupt,), client=None):
        self.type = type
        self.id = id
        self.abort_exceptions = abort_exceptions
        self.finalize = finalize
        self.client = client
        self.printer = PrintOperationInfo(id, client=client)

        proxy_url = get_proxy_url(check=False, client=self.client)
        if proxy_url:
            self.url = \
                get_config(self.client)["proxy"]["operation_link_pattern"]\
                    .format(proxy=get_proxy_url(client=self.client), id=self.id)
        else:
            self.url = None

    def suspend(self):
        """Suspend operation. """
        suspend_operation(self.id, client=self.client)

    def resume(self):
        """Resume operation. """
        resume_operation(self.id, client=self.client)

    def abort(self):
        """Abort operation. """
        abort_operation(self.id, client=self.client)

    def complete(self):
        """Complete operation. """
        complete_operation(self.id, client=self.client)

    def get_state_monitor(self, time_watcher, action=lambda: None):
        """Returns iterator over operation progress states. """
        return get_operation_state_monitor(self.id, time_watcher, action, client=self.client)

    def get_attributes(self):
        """Returns all operation attributes. """
        return get_operation_attributes(self.id, client=self.client)

    def get_job_statistics(self):
        """Returns job statistics of operation. """
        try:
            return get("{0}/{1}/@progress/job_statistics".format(OPERATIONS_PATH, self.id), client=self.client)
        except YtResponseError as error:
            if error.is_resolve_error():
                return {}
            raise

    def get_progress(self):
        """Returns dictionary that represents number of different types of jobs. """
        return get_operation_progress(self.id, client=self.client)

    def get_state(self):
        """Returns object that represents state of operation. """
        return get_operation_state(self.id, client=self.client)

    def get_stderrs(self, only_failed_jobs=False):
        """Returns list of objects thar represents jobs with stderrs.
        Each object is dict with keys "stderr", "error" (if applyable), "host".

        :param only_failed_jobs: (bool) consider only failed jobs.
        """
        return get_stderrs(self.id, only_failed_jobs=only_failed_jobs, client=self.client)

    def exists(self):
        """Check if operation attributes can be fetched from Cypress."""
        try:
            self.get_attributes()
        except YtResponseError as err:
            if err.is_resolve_error():
                return False
            raise

        return True

    def wait(self, check_result=True, print_progress=True, timeout=None):
        """Synchronously track operation, print current progress and finalize at the completion.

        If timeout occurred, raise `YtTimeoutError`.
        If operation failed, raise `YtOperationFailedError`.
        If `KeyboardInterrupt` occurred, abort operation, finalize and reraise `KeyboardInterrupt`.

        :param check_result: (bool) get stderr if operation failed
        :param print_progress: (bool)
        :param timeout: (double) timeout of operation in sec. ``None`` means operation is endlessly waited for.
        """

        finalize = self.finalize if self.finalize else lambda state: None
        operation_poll_period = get_config(self.client)["operation_tracker"]["poll_period"] / 1000.0
        time_watcher = TimeWatcher(min_interval=operation_poll_period / 5.0,
                                   max_interval=operation_poll_period,
                                   slowdown_coef=0.1, timeout=timeout)
        print_info = self.printer if print_progress else lambda state: None

        def abort():
            for state in self.get_state_monitor(TimeWatcher(1.0, 1.0, 0.0, timeout=None), self.abort):
                print_info(state)
            finalize(state)


        with ExceptionCatcher(self.abort_exceptions, abort, enable=get_config(self.client)["operation_tracker"]["abort_on_sigint"]):
            for state in self.get_state_monitor(time_watcher):
                print_info(state)
            timeout_occurred = time_watcher.is_time_up()
            finalize(state)
            if timeout_occurred:
                logger.info("Timeout occurred.")
                raise YtTimeoutError()

        if check_result and state.is_unsuccessfully_finished():
            raise _create_operation_failed_error(self, state)

        if get_config(self.client)["operation_tracker"]["log_job_statistics"]:
            statistics = self.get_job_statistics()
            if statistics:
                logger.info("Job statistics:\n" + yson.dumps(self.get_job_statistics(), yson_format="pretty"))

        stderr_level = logging._levelNames[get_config(self.client)["operation_tracker"]["stderr_logging_level"]]
        if logger.LOGGER.isEnabledFor(stderr_level):
            stderrs = get_stderrs(self.id, only_failed_jobs=False, client=self.client)
            if stderrs:
                logger.log(stderr_level, "\n" + format_operation_stderrs(stderrs))

class OperationsTracker(object):
    """Holds operations and allows to wait or abort all tracked operations."""
    def __init__(self, poll_period=5000, abort_on_sigint=True):
        self._operations = {}
        self._poll_period = poll_period
        self._abort_on_sigint = abort_on_sigint

    def add(self, operation):
        """Adds Operation object to tracker.
        :param operation: (Operation) operation to track
        """
        if not isinstance(operation, Operation):
            raise YtError("Valid Operation object should be passed "
                          "to add method, not {0}".format(repr(operation)))

        if not operation.exists():
            raise YtError("Operation %s does not exist and is not added", operation.id)
        self._operations[operation.id] = operation

    def add_by_id(self, operation_id, client=None):
        """Adds operation to tracker (by operation id)
        :param operation_id: (str) operation id
        """
        try:
            attributes = get_operation_attributes(operation_id, client=client)
            operation = Operation(attributes["operation_type"], operation_id, client=client)
            self._operations[operation_id] = operation
        except YtResponseError as err:
            if err.is_resolve_error():
                raise YtError("Operation %s does not exist and is not added", operation_id)
            raise

    def wait_all(self, check_result=True, print_progress=True, abort_exceptions=(KeyboardInterrupt,)):
        """Waits all added operations and prints progress.
        :param check_result: (bool) if True then `YtError` will be raised if any operation failed.
        For each failed operation `YtOperationFailedError` object with details will be added to raised error.
        :param print_progress: (bool)
        """
        logger.info("Waiting for %d operations to complete...", len(self._operations))

        unsucessfully_finished_count = 0
        inner_errors = []

        with ExceptionCatcher(abort_exceptions, self.abort_all, enable=self._abort_on_sigint):
            while self._operations:
                operations_to_remove = []

                for id_, operation in iteritems(self._operations):
                    state = operation.get_state()

                    if print_progress:
                        operation.printer(state)

                    if state.is_finished():
                        operations_to_remove.append(id_)

                        if state.is_unsuccessfully_finished():
                            unsucessfully_finished_count += 1
                            inner_errors.append(_create_operation_failed_error(operation, state))

                for operation_id in operations_to_remove:
                    del self._operations[operation_id]

                sleep(self._poll_period / 1000.0)

        if check_result and unsucessfully_finished_count > 0:
            raise YtError("All tracked operations finished but {0} operations finished unsucessfully"
                          .format(unsucessfully_finished_count), inner_errors=inner_errors)

    def abort_all(self):
        """Aborts all added operations."""
        logger.info("Aborting %d operations", len(self._operations))
        for id_, operation in iteritems(self._operations):
            state = operation.get_state()

            if state.is_finished():
                logger.warning("Operation %s is in %s state and cannot be aborted", id_, state)
                continue

            operation.abort()
            logger.info("Operation %s was aborted", id_)

        self._operations.clear()
