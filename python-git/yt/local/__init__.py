""" CLI and Python interface for YT local mode """
from .commands import start, stop, delete, get_proxy, list_instances
from .helpers import YTCheckingThread

class LocalYt(object):
    def __init__(self, *args, **kwargs):
        self._args = args
        self._kwargs = kwargs
        self._local_yt_instance = None

    def __enter__(self):
        assert self._local_yt_instance is None
        self._local_yt_instance = start(*self._args, **self._kwargs)
        return self._local_yt_instance.create_client()

    def __exit__(self, exc_type, exc_val, exc_tb):
        assert self._local_yt_instance is not None
        stop(self._local_yt_instance.id)
        self._local_yt_instance = None
