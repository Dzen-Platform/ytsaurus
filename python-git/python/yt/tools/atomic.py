import yt.logger as logger
import yt.wrapper as yt

import __builtin__

import random
import sys
from time import sleep

def atomic_pop(list, retries_count=10, delay=5.0):
    with yt.Transaction():
        for i in xrange(retries_count):
            logger.info("Trying to take lock, %d-th attempt...", i + 1)
            try:
                count = int(yt.get(list + "/@count"))
                if not count:
                    return
                yt.lock(list + "/-1")
                value = yt.get(list + "/-1")
                yt.remove(list + "/-1", recursive=True)
                return value
            # We hope that it is cannot take lock error
            except yt.YtResponseError as rsp:
                if rsp.is_concurrent_transaction_lock_conflict():
                    timeout = random.uniform(0.1, delay)
                    logger.info("Lock conflict, waiting for %f second...", timeout)
                    sleep(timeout)
                else:
                    raise


def atomic_push(list, value):
    logger.warning("Put value '%s' to queue '%s'", str(value), list)
    yt.set(list + "/begin", value)

def is_hashable(obj):
    try:
        hash(obj)
        return True
    except:
        return False

def process_tasks_from_list(list, action, limit=10000):
    processed_values = set()
    counter = 0
    while True:
        value = None
        try:
            value = atomic_pop(list)

            if value is None:
                logger.info("Queue '%s' is empty, processing stopped", list)
                break

            hashable_value = value
            if isinstance(value, __builtin__.list):
                hashable_value = tuple(value)
            elif isinstance(value, __builtin__.dict):
                hashable_value = tuple(value.items())

            if hashable_value is not None and is_hashable(hashable_value):
                if hashable_value in processed_values:
                    logger.info("We have already prosessed value '%s', processing stopped", str(value))
                    atomic_push(list, value)
                    break
                processed_values.add(hashable_value)

            logger.info("Processing value %s", str(value))
            result = action(value)
            if result == -1:
                atomic_push(list, value)

        except (Exception, KeyboardInterrupt):
            logger.exception("Process interrupted or error occured, processing stopped")
            if value is not None:
                atomic_push(list, value)
            break

        counter += 1
        if counter == limit:
            logger.warning("Too many values are processed (%d), aborting", limit)
            break

