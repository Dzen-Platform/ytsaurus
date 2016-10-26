from .config import get_option, set_option, get_config, get_total_request_timeout, get_request_retry_count
import yt.logger as logger
from .common import get_value
from .errors import YtResponseError, YtError, YtTransactionPingError
from .transaction_commands import start_transaction, commit_transaction, abort_transaction, ping_transaction

from yt.packages.six.moves._thread import interrupt_main

from time import sleep
from threading import Thread
from datetime import datetime, timedelta
import signal
import os

null_transaction_id = "0-0-0-0"

_sigusr_received = False

def _set_sigusr_received(value):
    global _sigusr_received
    _sigusr_received = value

def _sigusr_handler(signum, frame):
    # XXX(asaitgalin): handler is always executed in main thread so it is safe
    # to raise error here. This error will be caught by __exit__ method so it is ok.
    if signum == signal.SIGUSR1 and not _sigusr_received:
        _set_sigusr_received(True)
        raise YtTransactionPingError()

class TransactionStack(object):
    def __init__(self):
        self._stack = []
        self.initial_transaction = null_transaction_id
        self.initial_ping_ancestor_transactions = False

    def init(self, initial_transaction, initial_ping_ancestor_transactions):
        if not self._stack:
            self.initial_transaction = initial_transaction
            self.initial_ping_ancestor_transactions = initial_ping_ancestor_transactions

    def append(self, transaction_id, ping_ancestor_transactions):
        self._stack.append((transaction_id, ping_ancestor_transactions))

    def pop(self):
        self._stack.pop()

    def get(self):
        if self._stack:
            return self._stack[-1]
        else:
            return self.initial_transaction, self.initial_ping_ancestor_transactions


class Transaction(object):
    """
    It is designed to be used by with_statement::

    >>> with Transaction():
    >>>    ...
    >>>    lock("//home/my_node")
    >>>    ...
    >>>    with Transaction():
    >>>        ...
    >>>        yt.run_map(...)
    >>>

    Caution: if you use this class then do not use directly methods *_transaction.

    .. seealso:: `transactions on wiki <https://wiki.yandex-team.ru/yt/userdoc/transactions>`_
    """

    def __init__(self, timeout=None, attributes=None, ping=True, interrupt_on_failed=True, transaction_id=None,
                 ping_ancestor_transactions=False, type="master", sticky=False,
                 client=None):
        timeout = get_value(timeout, get_total_request_timeout(client))
        if transaction_id == null_transaction_id:
            ping = False

        self.transaction_id = transaction_id
        self.sticky = sticky

        self._client = client
        self._ping = ping
        self._ping_ancestor_transactions = ping_ancestor_transactions
        self._finished = False

        if get_option("_transaction_stack", self._client) is None:
            set_option("_transaction_stack", TransactionStack(), self._client)
        self._stack = get_option("_transaction_stack", self._client)
        self._stack.init(get_option("TRANSACTION", self._client), get_option("PING_ANCESTOR_TRANSACTIONS", self._client))

        if self.transaction_id is None:
            self.transaction_id = start_transaction(timeout=timeout,
                                                    attributes=attributes,
                                                    type=type,
                                                    sticky=sticky,
                                                    client=self._client)
            self._started = True
        else:
            self._started = False

        if get_config(self._client)["transaction_use_signal_if_ping_failed"]:
            _set_sigusr_received(False)
            self._old_sigusr_handler = signal.signal(signal.SIGUSR1, _sigusr_handler)

        if self._ping and self._started:
            delay = (timeout / 1000.0) / max(2, get_request_retry_count(self._client))
            self._ping_thread = PingTransaction(
                self.transaction_id,
                delay,
                sticky=self.sticky,
                interrupt_on_failed=interrupt_on_failed,
                client=self._client)
            self._ping_thread.start()

    def abort(self):
        """Abort transaction."""
        if self._finished or self.transaction_id == null_transaction_id:
            return
        self._stop_pinger()
        abort_transaction(self.transaction_id, sticky=self.sticky, client=self._client)
        self._finished = True

    def commit(self):
        """Commit transaction."""
        if self.transaction_id == null_transaction_id:
            return
        if self._finished:
            raise YtError("Transaction is already finished, cannot commit")
        self._stop_pinger()
        commit_transaction(self.transaction_id, sticky=self.sticky, client=self._client)
        self._finished = True

    def is_pinger_alive(self):
        """Check pinger is alive."""
        if self._ping:
            return not self._ping_thread.failed
        return True

    def __enter__(self):
        self._stack.append(self.transaction_id, self._ping_ancestor_transactions)
        set_option("TRANSACTION", self.transaction_id, self._client)
        set_option("PING_ANCESTOR_TRANSACTIONS", self._ping_ancestor_transactions, self._client)
        return self

    def __exit__(self, type, value, traceback):
        if self._finished:
            return

        try:
            if not self._started or self.transaction_id == null_transaction_id:
                return

            action = "commit" if type is None else "abort"
            if action == "abort":
                # NB: logger may be socket logger. In this case we should convert error to string to avoid
                # bug with unpickling Exception that has non-trivial __init__.
                logger.warning(
                    "Error: (type=%s, value=%s), aborting transaction %s ...",
                    type,
                    str(value).replace("\n", "\\n"),
                    self.transaction_id)

            try:
                if action == "commit":
                    self.commit()
                else:
                    self.abort()
            except YtResponseError as rsp:
                if rsp.is_resolve_error():
                    logger.warning("Transaction %s is missing, cannot %s", self.transaction_id, action)
                    if action == "commit":
                        raise
                else:
                    raise
        finally:
            self._stack.pop()
            if get_config(self._client)["transaction_use_signal_if_ping_failed"]:
                signal.signal(signal.SIGUSR1, self._old_sigusr_handler)
            transaction_id, ping_ancestor_transactions = self._stack.get()
            set_option("TRANSACTION", transaction_id, self._client)
            set_option("PING_ANCESTOR_TRANSACTIONS", ping_ancestor_transactions, self._client)

    def _stop_pinger(self):
        if self._ping:
            self._ping_thread.stop()


class PingTransaction(Thread):
    """
    Pinger for transaction.

    Ping transaction in background thread.
    """
    def __init__(self, transaction, delay, sticky=False, interrupt_on_failed=True, client=None):
        """
        :param delay: delay in seconds
        """
        super(PingTransaction, self).__init__()
        self.transaction = transaction
        self.delay = delay
        self.sticky = sticky
        self.interrupt_on_failed = interrupt_on_failed
        self.failed = False
        self.is_running = True
        self.daemon = True
        self.step = min(self.delay, get_config(client)["transaction_sleep_period"] / 1000.0) # in seconds
        self._client = client

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, type, value, traceback):
        self.stop()

    def stop(self):
        if not self.is_running:
            return
        self.is_running = False
        timeout = get_total_request_timeout(self._client) / 1000.0
        # timeout should be enough to execute ping
        self.join(timeout + 2 * self.step)
        if self.is_alive():
            logger.warning("Ping request could not be completed within %.1lf seconds", timeout)

    def run(self):
        while self.is_running:
            try:
                ping_transaction(self.transaction, sticky=self.sticky, client=self._client)
            except:
                self.failed = True
                if self.interrupt_on_failed:
                    logger.exception("Ping failed")
                    if get_config(self._client)["transaction_use_signal_if_ping_failed"]:
                        os.kill(os.getpid(), signal.SIGUSR1)
                    else:
                        interrupt_main()
                else:
                    logger.warning("Failed to ping transaction %s, pinger stopped", self.transaction)
                    return
            start_time = datetime.now()
            while datetime.now() - start_time < timedelta(seconds=self.delay):
                sleep(self.step)
                if not self.is_running:
                    return

    def __del__(self):
        self.stop()

