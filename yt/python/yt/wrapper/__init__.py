"""
Python wrapper for HTTP-interface of YT system.

Package supports `YT API <https://yt.yandex-team.ru/docs/api/python/python_wrapper.html>`_.

Be ready to catch :class:`YtError <yt.common.YtError>` after all commands!
"""

from . import version_check

from .idm_client import YtIdmClient

from .client_api import *
from .client import YtClient, create_client_with_command_params

from .spec_builders import (
    JobIOSpecBuilder, PartitionJobIOSpecBuilder, SortJobIOSpecBuilder, MergeJobIOSpecBuilder, ReduceJobIOSpecBuilder,
    MapJobIOSpecBuilder,
    UserJobSpecBuilder, TaskSpecBuilder, MapperSpecBuilder, ReducerSpecBuilder, ReduceCombinerSpecBuilder,
    ReduceSpecBuilder, JoinReduceSpecBuilder, MapSpecBuilder, MapReduceSpecBuilder, MergeSpecBuilder,
    SortSpecBuilder, RemoteCopySpecBuilder, EraseSpecBuilder, VanillaSpecBuilder)
from .errors import (YtError, YtOperationFailedError, YtResponseError, YtHttpResponseError,
                     YtProxyUnavailable, YtTokenError, YtTransactionPingError, YtRequestTimedOut)
from .batch_execution import YtBatchRequestFailedError
from .yamr_record import Record
from .format import (DsvFormat, YamrFormat, YsonFormat, JsonFormat, SchemafulDsvFormat, SkiffFormat,
                     YamredDsvFormat, Format, create_format, dumps_row, loads_row, YtFormatError, create_table_switch)
from .ypath import YPath, TablePath, FilePath, ypath_join, ypath_dirname, ypath_split, escape_ypath_literal
from .operation_commands import format_operation_stderrs, Operation
from .operations_tracker import OperationsTracker, OperationsTrackerPool
from .py_wrapper import (aggregator, raw, raw_io, reduce_aggregator,
                         enable_python_job_processing_for_standalone_binary, initialize_python_job_processing,
                         with_context, with_skiff_schemas)
from .string_iter_io import StringIterIO
from .user_statistics import write_statistics
from .yamr_mode import set_yamr_mode
from .dynamic_table_commands import ASYNC_LAST_COMMITED_TIMESTAMP, SYNC_LAST_COMMITED_TIMESTAMP
from .skiff import convert_to_skiff_schema
from .http_helpers import get_retriable_errors

from .common import get_version, is_inside_job, escape_c
__version__ = VERSION = get_version()

# Some usefull parts of private API.
from .http_helpers import (_cleanup_http_session,
                           get_token as _get_token,
                           get_proxy_url as _get_proxy_url,
                           make_request_with_retries as _make_http_request_with_retries)

# For PyCharm checks
from . import config
from . import default_config
from .config import update_config
