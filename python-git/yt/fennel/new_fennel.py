from misc import _convert_to_tskved_json, LOGBROKER_TSKV_PREFIX

import yt.packages.requests as requests

import yt.wrapper as yt

import os
import sys
import time
import socket
import logging
import struct
from datetime import datetime
from cStringIO import StringIO
from gzip import GzipFile
from itertools import starmap
from threading import Thread
from Queue import Queue

# Brief description.
# Attributes on table:
# processed_row_count - number of rows in current table pushed to logbroker.
# archived_row_count - number of archived rows.
# last_saved_ts - timestamp of last record pushed to logbroker.
# table_start_row_index - number of processed rows before the beginning of table.
# Should be correctly recalculated by rotation script.
#
# Each row in table should have 'timestamp' column

# ================= Global options =============================================

ATTRIBUTES_UPDATE_ATTEMPTS = 10
ATTRIBUTES_UPDATE_FAILURE_BACKOFF = 20
PUSH_FAILURE_BACKOFF = 20
LOCK_ACQUIRE_BACKOFF = 10

# ================= Common stuff ===============================================

class FennelError(Exception):
    pass

logger = logging.getLogger("Fennel")

# ================= StringIO wrapper ===========================================

class StringIOWrapper(object):
    def __init__(self):
        self.underlying = StringIO()
        self.size = 0

    def write(self, data):
        self.underlying.write(data)
        self.size += len(data)

    def getvalue(self):
        return self.underlying.getvalue()

# ================= COMMON HELPERS =============================================

def round_down_to(num, modulo):
    if num % modulo == 0:
        return num
    else:
        return num - (num % modulo)

def raise_for_status(response):
    if not response.ok:
        logger.error("Request to %s failed with code %s and body %r", response.url, response.status_code, response.text)
        response.raise_for_status()

# ================= LOGBROKER HELPERS ==========================================

class LogBroker(object):
    def __init__(self, url, log_type, service_id, source_id, chunk_size):
        self._url = url
        self._log_type = log_type
        self._service_id = service_id
        self._source_id = source_id
        self._chunk_size = chunk_size

        self._cached_url = None

    def _make_source_id(self, session_index, session_count):
        return "{0}_{1}_{2}".format(self._source_id, session_count, session_index + 1)

    def get_chunk_size(self):
        return self._chunk_size

    def get_session_options(self, session_index, session_count):
        return (self._request_logbroker_hostname(session_index, session_count),
                self._get_session_params(session_index, session_count))

    def _get_session_params(self, session_index, session_count):
        return {"ident": self._service_id, "logtype": self._log_type, "sourceid": self._make_source_id(session_index, session_count)}

    def _request_logbroker_hostname(self, session_index, session_count):
        logger.info("Getting adviced logbroker endpoint hostname for %s...", self._url)

        headers = {"ClientHost": socket.getfqdn(), "Accept-Encoding": "identity"}
        params = self._get_session_params(session_index, session_count)
        response = requests.get("http://{0}/advise".format(self._url), params=params, headers=headers)
        raise_for_status(response)

        host = response.text.strip()

        logger.info("Adviced endpoint hostname: %s", host)

        return host

    def _get_url(self, session_index, session_count):
        if self._cached_url is None:
            self._cached_url = self._request_logbroker_hostname()
        return self._cached_url

# ================= MONITOR ====================================================

def monitor(yt_client, table_path, threshold, output=sys.stdout):
    """ threshold in hours """
    try:
        now = datetime.utcnow()
        last_processed_time_str = yt_client.get(table_path + "/@last_saved_ts")
        last_processed_time = datetime.strptime(last_processed_time_str, "%Y-%m-%d %H:%M:%S")
        lag = now - last_processed_time
    except Exception:
        logger.error("Failed to run monitoring. Unhandled exception", exc_info=True)
        output.write("2; Internal error\n")
    else:
        if lag.total_seconds() > threshold * 60:
            output.write("2; Lag equals to: %s\n" % (lag,))
        else:
            output.write("0; Lag equals to: %s\n" % (lag,))

# ================= PUSH TO LOGBROKER  =========================================

def write_header(seqno, stream):
    # v2le header format: <chunk_id><seqno><lines>
    # lines is used just for debug, we do not use it.
    stream.write(struct.pack("<QQQ", seqno, seqno, 0))

def write_row(row, stream):
    format = yt.DsvFormat(enable_escaping=True)
    with GzipFile(fileobj=stream, mode="a") as zipped_stream:
        zipped_stream.write(LOGBROKER_TSKV_PREFIX)
        format.dump_row(_convert_to_tskved_json(row), zipped_stream)


def convert_rows_to_chunks(rows, chunk_size, seqno):
    stream = StringIOWrapper()
    has_data = False

    count = 0
    for row in rows:
        seqno += 1
        if not has_data:
            write_header(seqno, stream)
            count = 0
            has_data = True
        write_row(row, stream)
        count += 1
        if stream.size > chunk_size:
            yield stream.getvalue(), count
            stream = StringIOWrapper()
            has_data = False

    if has_data:
        yield stream.getvalue(), count

def make_yt_table_range(start_row_index, end_row_index):
    return {"lower_limit": {"row_index": start_row_index}, "upper_limit": {"row_index": end_row_index}}

def shift_ranges(ranges, table_start_index):
    return [(start + table_start_index, end + table_start_index) for start, end in ranges]

def strip_ranges(ranges, min_index, max_index):
    stripped_ranges = []
    for start, end in ranges:
        if start >= max_index:
            break
        start = max(start, min_index)
        end = min(end, max_index)
        if start >= end:
            continue
        stripped_ranges.append((start, end))

    return stripped_ranges

def create_global_ranges_for_session(start_row_index, end_row_index, range_size, session_index, session_count, max_range_count):
    step = session_count * range_size
    start = round_down_to(start_row_index, range_size * session_count) + session_index * range_size
    end_row_index = min(end_row_index, start_row_index + max_range_count * step)
    ranges = [(index, index + range_size) for index in xrange(start, start + step * (max_range_count + 1), step)]
    ranges = strip_ranges(ranges, start_row_index, end_row_index)
    return ranges[:max_range_count]

def make_read_tasks(yt_client, table_path, session_count, range_row_count, max_range_count):
    attributes = yt_client.get(table_path + "/@")
    if "processed_row_count" not in attributes:
        with yt_client.Transaction(transaction_id="0-0-0-0"):
            yt_client.set("{0}/@{1}".format(table_path, "processed_row_count"), 0)
            yt_client.set("{0}/@{1}".format(table_path, "table_start_row_index"), 0)
        attributes = yt_client.get(table_path + "/@")

    row_count = attributes["row_count"]
    processed_row_count = attributes["processed_row_count"]
    table_start_row_index = attributes["table_start_row_index"]

    logger.info("Building tasks (row_count: %d, processed_row_count: %d, table_start_row_index: %d)",
                row_count, processed_row_count, table_start_row_index)

    tasks = []
    all_ranges = []
    for session_index in xrange(session_count):
        ranges = create_global_ranges_for_session(
            start_row_index=table_start_row_index + processed_row_count,
            end_row_index=table_start_row_index + row_count,
            range_size=range_row_count,
            session_index=session_index,
            session_count=session_count,
            max_range_count=max_range_count)

        if not ranges:
            continue

        all_ranges += ranges

        start_row_index = ranges[0][0]
        seqno = round_down_to(start_row_index, session_count * range_row_count) / session_count + \
            (start_row_index - round_down_to(start_row_index, range_row_count))

        ranges = shift_ranges(ranges, -table_start_row_index)
        ranges = list(starmap(make_yt_table_range, ranges))
        tasks.append((session_index, seqno, ranges))

    tasks_row_count = sum(end - start for start, end in all_ranges)

    all_ranges.sort()
    for current, next in zip(all_ranges, all_ranges[1:]):
        assert current[1] == next[0]

    return tasks, tasks_row_count

def pipe_from_yt_to_logbroker(yt_client, logbroker, table_path, ranges, session_index, session_count, seqno):
    url, params = logbroker.get_session_options(session_index, session_count)
    session_rsp = requests.get("http://{}/rt/session".format(url), params=params, stream=True)
    raise_for_status(session_rsp)

    session_rsp_lines_iter = session_rsp.iter_lines()
    session_id = session_rsp.headers["Session"]

    logger.info("Session started with id %s", session_id)

    rows = yt_client.read_table(yt.TablePath(table_path, ranges=ranges, simplify=False))
    def gen_data():
        logger.info("Start write session with seqno: %d", seqno)
        for chunk, row_count in convert_rows_to_chunks(rows, logbroker.get_chunk_size(), seqno):
            logger.info("Writing chunk (size: %d, row_count: %d)", len(chunk), row_count)
            yield chunk

            while True:
                line = session_rsp_lines_iter.next()
                if line.startswith("ping"):
                    continue
                if any(line.startswith(marker) for marker in ["eof", "error", "dc-off"]):
                    raise FennelError("Failed to write chunk: received message %r", line)
                if line.startswith("skip"):
                    logger.info("Chunk skipped due to duplication, response: %s", line)
                    break

                if line.startswith("chunk"):
                    # TODO(ignat): add more checks
                    logger.info("Chunk successfully written, response: %s", line)
                    break

    store_rsp = requests.put("http://{}:9000/rt/store".format(url),
                             headers={
                                 "Session": session_id,
                                 "RTSTreamFormat": "v2le",
                                 "Content-Encoding": "gzip"
                             },
                             data=gen_data(),
                             stream=True)
    raise_for_status(store_rsp)
    store_rsp.close()

    session_rsp.close()

class PushMapper(object):
    def __init__(self, yt_client, logbroker):
        self.yt_client = yt_client
        self.logbroker = logbroker

    def __call__(self, row):
        del row["@table_index"]
        logger.handlers = [logging.StreamHandler()]
        logger.setLevel(logging.INFO)
        pipe_from_yt_to_logbroker(self.yt_client, self.logbroker, **row)
        if False:
            yield

def update_table_attributes(yt_client, table_path, pushed_row_count):
    with yt_client.Transaction():
        yt_client.lock(table_path, mode="exclusive")
        processed_row_count = yt_client.get(table_path + "/@processed_row_count")
        yt_client.set(table_path + "/@processed_row_count", processed_row_count + pushed_row_count)

        last_row = yt_client.read_table(yt.TablePath(table_path, exact_index=processed_row_count + pushed_row_count - 1)).next()
        if "timestamp" in last_row:
            yt_client.set(table_path + "/@last_saved_ts", last_row["timestamp"])

def push_to_logbroker_one_portion(yt_client, logbroker, table_path, session_count, range_row_count, max_range_count):
    pushed_row_count = 0
    with yt_client.Transaction():
        yt_client.lock(table_path, mode="snapshot")

        tasks, pushed_row_count = make_read_tasks(yt_client, table_path, session_count, range_row_count, max_range_count)
        if not tasks:
            return

        if session_count == 1:
            logger.info("Push data locally")
            assert len(tasks) == 1
            session_index, seqno, ranges = tasks[0]
            pipe_from_yt_to_logbroker(yt_client, logbroker, table_path, ranges, session_index, session_count, seqno)
        else:
            with yt_client.TempTable() as input_table, yt_client.TempTable() as output_table:
                rows = []
                for task in tasks:
                    session_index, seqno, ranges = task
                    rows.append({"table_path": table_path, "ranges": ranges, "session_index": session_index, "session_count": session_count, "seqno": seqno})
                yt_client.write_table(input_table, rows)

                logger.info("Starting push operation (input: %s, output: %s, task_count: %d)", input_table, output_table, len(tasks))
                yt_client.config["allow_http_requests_to_yt_from_job"] = True
                yt_client.config["pickling"]["module_filter"] = lambda module: hasattr(module, "__file__") and not "raven" in module.__file__
                yt_client.run_map(PushMapper(yt_client, logbroker), input_table, output_table, spec={"data_size_per_job": 1})
                logger.info("Push operation successfully finished (pushed_row_count: %d)", pushed_row_count)

    for iter in xrange(ATTRIBUTES_UPDATE_ATTEMPTS):
        try:
            update_table_attributes(yt_client, table_path, pushed_row_count)
            break
        except yt.YtResponseError as err:
            if not err.is_concurrent_transaction_lock_conflict():
                raise
            logger.info("Failed to update attributes of %s since lock conflict (attempt: %d, backoff: %d seconds)",
                        table_path, iter, ATTRIBUTES_UPDATE_FAILURE_BACKOFF)
            time.sleep(ATTRIBUTES_UPDATE_FAILURE_BACKOFF)

def acquire_yt_lock(yt_client, lock_path, queue):
    try:
        yt_client.create(lock_path, ignore_existing=False)
        with yt_client.Transaction() as tx:
            logger.info("Acquiring lock under tx %s", tx.transaction_id)
            while True:
                try:
                    yt_client.lock(lock_path)
                    break
                except yt.YtError as err:
                    if err.is_concurrent_transaction_lock_conflict():
                        logger.info("Failed to take lock")
                        time.sleep(LOCK_ACQUIRE_BACKOFF)
                        continue
                    logger.exception(str(err))
                    raise

            logger.info("Lock acquired")

            queue.put(True)

            # Sleep infinitely long
            while True:
                time.sleep(LOCK_ACQUIRE_BACKOFF)
    except:
        os._exit(1)


def push_to_logbroker(yt_client, logbroker, daemon, table_path, session_count, range_row_count, max_range_count, sentry_endpoint, lock_path):
    try:
        from raven.handlers.logging import SentryHandler
    except ImportError:
        SentryHandler = None

    if sentry_endpoint and SentryHandler is not None:
        root_logger = logging.getLogger("")
        sentry_handler = SentryHandler(sentry_endpoint)
        sentry_handler.setLevel(logging.ERROR)
        root_logger.addHandler(sentry_handler)
    
    if lock_path:
        lock_queue = Queue()
        lock_args = dict(yt_client=yt_client, lock_path=lock_path, queue=lock_queue)
        lock_thread = Thread(target=acquire_yt_lock, kwargs=lock_args, name="LockProcess")
        lock_thread.daemon = True
        lock_thread.start()
        lock_queue.get()

    options = dict(yt_client=yt_client, logbroker=logbroker, table_path=table_path, session_count=session_count, range_row_count=range_row_count, max_range_count=max_range_count)

    if daemon:
        while True:
            try:
                push_to_logbroker_one_portion(**options)
            except (yt.YtError, FennelError):
                logger.exception("Push failed, sleep and try again")
                time.sleep(PUSH_FAILURE_BACKOFF)
    else:
        push_to_logbroker_one_portion(**options)

