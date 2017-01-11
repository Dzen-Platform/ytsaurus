"""
Python wrapper for HTTP-interface of YT system.

Package supports `YT API <https://wiki.yandex-team.ru/yt/pythonwrapper>`_.

Be ready to catch :class:`YtError <yt.common.YtError>` after all commands!
"""

from . import version_check

from .client_api import *
from .client import YtClient

from .errors import YtError, YtOperationFailedError, YtResponseError, YtHttpResponseError, \
                    YtProxyUnavailable, YtTokenError, YtTransactionPingError
from .yamr_record import Record
from .format import DsvFormat, YamrFormat, YsonFormat, JsonFormat, SchemafulDsvFormat,\
                    YamredDsvFormat, Format, create_format, dumps_row, loads_row, YtFormatError, create_table_switch
from .ypath import YPath, TablePath, FilePath, ypath_join
from .cypress_commands import escape_ypath_literal
from .operation_commands import format_operation_stderrs, Operation, OperationsTracker
from .py_wrapper import aggregator, raw, raw_io, reduce_aggregator, \
                        enable_python_job_processing_for_standalone_binary, initialize_python_job_processing, \
                        with_context
from .string_iter_io import StringIterIO
from .user_statistics import write_statistics, get_blkio_cgroup_statistics, get_memory_cgroup_statistics
from .yamr_mode import set_yamr_mode

from .common import get_version, is_inside_job
__version__ = VERSION = get_version()

# Some usefull parts of private API.
from .http_helpers import \
    _cleanup_http_session, \
    get_token as _get_token, \
    get_proxy_url as _get_proxy_url, \
    make_request_with_retries as _make_http_request_with_retries, \
    get_retriable_errors as _get_retriable_errors

# For PyCharm checks
from . import config
from .config import update_config
