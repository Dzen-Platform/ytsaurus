from yt.common import YtError
from yt.wrapper.common import get_value, require, update, run_with_retries, generate_uuid, bool_to_string
from yt.wrapper.http_helpers import get_retriable_errors, get_token, configure_ip
from yt.wrapper.errors import hide_token
from yt.wrapper.client import YtClient
import yt.logger as logger

import yt.packages.requests as requests
import yt.packages.simplejson as json

import sys
import time
import Queue
from threading import Thread, Semaphore
from copy import deepcopy

TM_BACKEND_URL = "http://transfer-manager.yt.yandex.net/api/v1"
TM_TASK_URL_PATTERN = "https://transfer-manager.yt.yandex-team.ru/task?id={id}&tab=details&backend={backend_tag}"

TM_HEADERS = {
    "Accept-Type": "application/json",
    "Content-Type": "application/json"
}

def get_version():
    try:
        from version import VERSION
        return VERSION
    except:
        return "unknown"

class TransferManagerUnavailableError(YtError):
    pass

class RequestIsBeingProcessedError(YtError):
    pass

def _raise_for_status(response):
    if response.status_code == 200:
        return

    if response.status_code == 500 or response.status_code == 502:
        message = "Transfer Manager is not available"
        if response.content:
            message += ": " + response.content

        raise TransferManagerUnavailableError(message)

    if response.status_code == 503:
        raise RequestIsBeingProcessedError(response.content)

    try:
        response_json = response.json()
    except ValueError as error:
        raise YtError("Cannot parse JSON from body '{0}'".format(response.content),
                      inner_errors=[YtError(error.message)])

    raise YtError(**response_json)

class Poller(object):
    def __init__(self, poll_period, running_tasks_limit, ping_task_and_get_func):
        self.poll_period = poll_period
        self.ping_task_and_get_func = ping_task_and_get_func

        self.exc_info = None

        self._thread = Thread(target=self._poll_tasks)
        self._thread.daemon = True
        self._thread.start()

        self._queue = Queue.Queue()
        self._semaphore = Semaphore(running_tasks_limit)

    def stop(self):
        self._queue.put({"type": "stop", "value": None})

        # XXX(asaitgalin): join() can't be interrupted with KeyboardInterrupt
        # so it is better to use polling with timeout.
        while self._thread.is_alive():
            self._thread.join(1.0)

        if self.exc_info is not None:
            raise self.exc_info[0], self.exc_info[1], self.exc_info[2]

        aborted_task_count, failed_task_count = self._queue.get()["value"]
        return aborted_task_count, failed_task_count

    def acquire_task_slot(self):
        return self._semaphore.acquire(False)

    def notify_task_started(self, task_id):
        self._queue.put({"type": "task", "value": task_id})

    def _poll_tasks(self):
        logger.info("Polling thread started...")

        is_running = True
        running_tasks = []

        aborted_task_count = 0
        failed_task_count = 0

        while is_running or running_tasks:
            tasks_to_remove = []

            for task in running_tasks:
                try:
                    state = self.ping_task_and_get_func(task)["state"]
                except:
                    self.exc_info = sys.exc_info()
                    return

                if state == "completed":
                    logger.info("Task %s completed", task)
                elif state == "skipped":
                    logger.info("Task %s skipped", task)
                elif state == "aborted":
                    logger.warning("Task {0} was aborted".format(task))
                    aborted_task_count += 1
                elif state == "failed":
                    logger.warning("Task {0} failed. Use get_task_info for more info".format(task))
                    failed_task_count += 1
                else:
                    continue

                tasks_to_remove.append(task)
                self._semaphore.release()

            time.sleep(self.poll_period)

            for task in tasks_to_remove:
                running_tasks.remove(task)

            while True:
                try:
                    msg = self._queue.get_nowait()
                except Queue.Empty:
                    break

                if msg["type"] == "stop":
                    is_running = False
                elif msg["type"] == "task":
                    running_tasks.append(msg["value"])
                else:
                    assert False, "Unknown message type {0}".format(msg["type"])

        # Send tasks statistics to main thread.
        self._queue.put({"type": "stats", "value": (aborted_task_count, failed_task_count)})

class TransferManager(object):
    def __init__(self, url=None, token=None, http_request_timeout=10000,
                 enable_retries=True, retry_count=6, force_ipv4=False,
                 force_ipv6=False):
        backend_url = get_value(url, TM_BACKEND_URL)

        # Backend url can be specified in short form.
        if backend_url.startswith("http://"):
            self.backend_url = backend_url
        else:
            self.backend_url = "http://{0}".format(backend_url)

        self.session = requests.Session()
        configure_ip(self.session, force_ipv4=force_ipv4, force_ipv6=force_ipv6)

        self.token = get_token(token=token)

        self.http_request_timeout = http_request_timeout
        self.enable_retries = enable_retries
        self.retry_count = retry_count

        self._backend_config = self.get_backend_config()

    def add_task(self, source_cluster, source_table, destination_cluster, destination_table=None, **kwargs):
        if "enable_early_skip_if_destination_exists" in kwargs:
            raise YtError('Argument "enable_early_skip_if_destination_exists" is not supported for single table copying')
        src_dst_pairs = [(source_table, destination_table)]
        return self.add_tasks_from_src_dst_pairs(src_dst_pairs, source_cluster, destination_cluster, **kwargs)[0]

    def add_tasks(self, source_cluster, source_pattern, destination_cluster, destination_pattern, **kwargs):
        src_dst_pairs = self.match_src_dst_pattern(source_cluster, source_pattern,
                                                   destination_cluster, destination_pattern)
        return self.add_tasks_from_src_dst_pairs(src_dst_pairs, source_cluster, destination_cluster, **kwargs)

    def abort_task(self, task_id):
        self._make_request(
            "POST",
            "{0}/tasks/{1}/abort/".format(self.backend_url, task_id),
            is_mutating=True)

    def restart_task(self, task_id):
        self._make_request(
            "POST",
            "{0}/tasks/{1}/restart/".format(self.backend_url, task_id),
            is_mutating=True)

    def get_task_info(self, task_id):
        return self._make_request("GET", "{0}/tasks/{1}/".format(self.backend_url, task_id)).json()

    def ping_task_and_get(self, task_id):
        url = "{0}/tasks/{1}/ping_and_get/".format(self.backend_url, task_id)
        return self._make_request("POST", url).json()

    def get_tasks(self, user=None, fields=None):
        params = {}
        if user is not None:
            params["user"] = user
        if fields is not None:
            params["fields[]"] = deepcopy(fields)

        return self._make_request("GET", "{0}/tasks/".format(self.backend_url), params=params).json()

    def get_backend_config(self):
        return self._make_request("GET", "{0}/config/".format(self.backend_url)).json()

    def match_src_dst_pattern(self, source_cluster, source_table, destination_cluster, destination_table):
        data = {
            "source_cluster": source_cluster,
            "source_pattern": source_table,
            "destination_cluster": destination_cluster,
            "destination_pattern": destination_table
        }

        return self._make_request(
            "POST",
            self.backend_url + "/match/",
            is_mutating=False,
            data=json.dumps(data)).json()

    def _make_request(self, method, url, is_mutating=False, **kwargs):
        headers = kwargs.get("headers", {})
        update(headers, TM_HEADERS)

        headers["User-Agent"] = "Transfer Manager client " + get_version()

        if method == "POST":
            require(self.token is not None, lambda: YtError("YT token is not specified"))
            headers["Authorization"] = "OAuth " + self.token

        params = {}
        if is_mutating:
            params["mutation_id"] = generate_uuid()
            params["retry"] = bool_to_string(False)

        def except_action(error):
            if is_mutating:
                if isinstance(error, TransferManagerUnavailableError):
                    params["mutation_id"] = generate_uuid()
                    params["retry"] = bool_to_string(False)
                else:
                    params["retry"] = bool_to_string(True)

        def backoff_action(error, iteration, sleep_backoff):
            logger.warning('HTTP %s request %s failed with error %s, message: "%s", headers: %s',
                           method, url, error.message, str(type(error)), str(hide_token(headers)))
            logger.warning("Sleep for %.2lf seconds before next retry (%d)", sleep_backoff, iteration + 1)

        def make_request():
            update(headers, {"X-TM-Parameters": json.dumps(params)})
            response = self.session.request(
                method,
                url,
                headers=headers,
                timeout=self.http_request_timeout / 1000.0,
                **kwargs)

            _raise_for_status(response)
            return response

        if self.enable_retries:
            retriable_errors = get_retriable_errors() + (TransferManagerUnavailableError,
                                                         RequestIsBeingProcessedError)
            return run_with_retries(make_request, self.retry_count, exceptions=retriable_errors,
                                    except_action=except_action, backoff_action=backoff_action)

        return make_request()

    def _start_one_task(self, source_table, source_cluster, destination_table, destination_cluster,
                        params=None):
        data = get_value(params, {})

        update(data, {
            "source_cluster": source_cluster,
            "source_table": source_table,
            "destination_cluster": destination_cluster
        })

        if destination_table is not None:
            data["destination_table"] = destination_table

        return self._make_request("POST",
                                  self.backend_url + "/tasks/",
                                  is_mutating=True,
                                  data=json.dumps(data)).content

    def add_tasks_from_src_dst_pairs(self, src_dst_pairs, source_cluster, destination_cluster, params=None,
                                     sync=None, poll_period=None, attached=False, running_tasks_limit=None,
                                     enable_early_skip_if_destination_exists=False):
        poll_period = get_value(poll_period, 5)
        running_tasks_limit = get_value(running_tasks_limit, 10)

        params = deepcopy(get_value(params, {}))
        if "lease_timeout" not in params and attached:
            params["lease_timeout"] = max(120, 2 * poll_period)

        tasks = []

        if sync:
            poller = Poller(poll_period, running_tasks_limit, self.ping_task_and_get)

        for source_table, destination_table in src_dst_pairs:
            if enable_early_skip_if_destination_exists and params.get("skip_if_destination_exists", False):
                cluster_info = self._backend_config["clusters"].get(destination_cluster)
                if cluster_info is None:
                    logger.warning("Cannot perform early task skipping: "
                                   "failed to retrieve cluster %s info from backend", destination_cluster)
                else:
                    if cluster_info["type"] != "yt":
                        logger.warning("Cannot perform early task skipping: it is supported only for YT clusters")
                    else:
                        client = YtClient(proxy=cluster_info["options"]["proxy"],
                                          token=params.get("destination_cluster_token", self.token))
                        if client.exists(destination_table):
                            logger.info("Skipped %s table since skip_if_destination_exists is set "
                                        "and destination table exists", destination_table)
                            continue

            if sync:
                while not poller.acquire_task_slot():
                    if poller.exc_info is not None:
                        raise poller.exc_info[0], poller.exc_info[1], poller.exc_info[2]
                    time.sleep(0.5)

            task_id = self._start_one_task(
                source_table,
                source_cluster,
                destination_table,
                destination_cluster,
                params=params)

            tasks.append(task_id)

            logger.info("Transfer task started: %s", TM_TASK_URL_PATTERN.format(
                id=task_id, backend_tag=self._backend_config["backend_tag"]))

            if sync:
                poller.notify_task_started(task_id)

        if sync:
            aborted_task_count, failed_task_count = poller.stop()

            if aborted_task_count or failed_task_count:
                raise YtError("All tasks done but there are {0} failed and {1} aborted tasks"
                              .format(failed_task_count, aborted_task_count))
            else:
                logger.info("All tasks successfully finished")

        return tasks

