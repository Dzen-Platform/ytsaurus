from __future__ import print_function

from errors import IncorrectTokenError

import yt.logger as logger

from yt.packages.six import iteritems

import yt.wrapper as yt

import os
import sys
import time
import traceback
import thread
import signal
import logging
import logging.handlers
from threading import Thread, current_thread

class SafeThread(Thread):
    def __init__(self, group=None, target=None, name=None, terminate=None, args=None, kwargs=None):
        self._parent_pid = os.getpid()

        if args is None:
            args = ()
        if kwargs is None:
            kwargs = {}

        def safe_run(*args, **kwargs):
            try:
                target(*args, **kwargs)
            except KeyboardInterrupt:
                print("Interrupting main from child thread:", current_thread(), file=sys.stderr)
                thread.interrupt_main()
                time.sleep(0.5)
                os.kill(self._parent_pid, signal.SIGTERM)
            except:
                print("Unknown exception", file=sys.stderr)
                print(traceback.format_exc(), file=sys.stderr)
                logger.exception("Unknown exception")
                os.kill(self._parent_pid, signal.SIGINT)
                time.sleep(0.5)
                os.kill(self._parent_pid, signal.SIGTERM)

        super(SafeThread, self).__init__(group=group, target=safe_run, name=name, args=args, kwargs=kwargs)

def log_yt_exception(logger, message=None):
    # Python2.7 has a bug in unpickling exception with non-trivial constructor.
    # To overcome it we convert exception to string before logging through socket handler.
    # https://bugs.python.org/issue1692335
    exc_type, exc_value, exc_traceback = sys.exc_info()
    exception_string = "\n".join(traceback.format_exception(exc_type, str(exc_value), exc_traceback))
    if message is not None:
        logger.error(message + "\n" + exception_string)
    else:
        logger.error(exception_string)

def _get_token(authorization_header):
    words = authorization_header.split()
    if len(words) != 2 or words[0].lower() != "oauth":
        return None
    return words[1]

def get_token_and_user(request, client):
    headers = {}
    for key, value in iteritems(request.headers):
        headers[key] = value

    # Some headers should not be passed.
    # NB: Passing Host header causes infinite redirect on locke
    for name in ["Host", "Content-Length"]:
        if name in headers:
            del headers[name]

    token = _get_token(headers.get("Authorization", ""))
    if "X-Forwarded-For" in headers:
        #headers["X-Forwarded-For"] += ", " + request.remote_addr
        # NB: it is not yet supported by proxy.
        pass
    else:
        headers["X-Forwarded-For"] = request.remote_addr
    if token == "undefined":
        user = "guest"
        token = ""
    else:
        user = client.get_user_name(token, headers=headers)
        if not user:
            raise IncorrectTokenError("Authorization token is incorrect: " + token)
    return token, user

def remove_unsigned(obj):
    def is_uint(num):
        return isinstance(num, long) and (num < -2 ** 63 or num >= 2 ** 63)

    if isinstance(obj, list):
        result = []
        for value in obj:
            new_value = remove_unsigned(value)
            if not is_uint(new_value):
                result.append(new_value)
        return result
    elif isinstance(obj, dict):
        result = {}
        for key, value in iteritems(obj):
            new_value = remove_unsigned(value)
            if not is_uint(new_value):
                result[key] = new_value
        return result
    else:
        return obj

def get_cluster_version(cluster_client):
    if hasattr(cluster_client, "_version"):
        return cluster_client._version

    if cluster_client._type == "yt":
        try:
            return cluster_client.get("//sys/@protocol_version")
        except yt.YtResponseError as err:
            if not err.is_resolve_error():
                raise

    return None

def configure_logging(logging_config):
    level = logging.__dict__[logging_config.get("level", "INFO")]
    if "filename" in logging_config:
        handler = logging.handlers.WatchedFileHandler(logging_config["filename"])
    else:
        handler = logging.StreamHandler()

    handler.setFormatter(logger.BASIC_FORMATTER)

    new_logger = logging.getLogger("Transfer Manager")
    new_logger.propagate = False
    new_logger.setLevel(level)
    new_logger.handlers = [handler]
    logger.LOGGER = new_logger
