from . import common
from .mappings import VerifiedDict

from yt.yson import YsonEntity

from copy import deepcopy

default_config = {
    # "http" | "native" | None
    # If backend equals "http", then all requests will be done through http proxy and http_config will be used.
    # If backend equals "native", then all requests will be done through c++ bindings and driver_config will be used.
    # If backend equals None, thenits value will be automatically detected.
    "backend": None,

    "retry_backoff": {
        # Policy of backoff for failed requests.
        # Supported values:
        # rounded_up_to_request_timeout - we will sleep due to the timeout of the request.
        #     For heavy requests we will sleep heave_request_timeout time.
        # constant_time - we will sleep time specified in constant time option.
        # exponential - we will sleep min(exponnetial_max_timeout, exponential_start_timeout * exponential_base ^ retry_attempt)
        "policy": "rounded_up_to_request_timeout",
        "constant_time": 1000.0,
        "exponential_start_timeout": 1000.0,
        "exponential_base": 2.0,
        "exponnetial_max_timeout": 60000.0,
    },

    # Configuration of proxy connection.
    "proxy": {
        "url": None,
        # Suffix appended to url if it is short.
        "default_suffix": ".yt.yandex.net",

        "accept_encoding": "gzip, identity",
        # "gzip" | "identity"
        "content_encoding": "gzip",

        # Number of retries and timeout between retries.
        "request_retry_timeout": 20000,
        "request_retry_count": 6,
        "request_retry_enable": True,

        # Heavy commands have increased timeout.
        "heavy_request_retry_timeout": 60000,

        # More retries in case of operation state discovery.
        "operation_state_discovery_retry_count": 100,

        # Forces backoff between consequent requests (for all requests, not just failed).
        # !!! It is not proxy specific !!!
        "request_backoff_time": None,

        "force_ipv4": False,
        "force_ipv6": False,

        # Format of header with yt parameters.
        # In new versions YT supports also "yson", that useful for passing unsinged int values.
        "header_format": None,

        # Enable using heavy proxies for heavy commands (write_*, read_*).
        "enable_proxy_discovery": True,
        # Number of top unbanned proxies that would be used to choose random
        # proxy for heavy request.
        "number_of_top_proxies_for_random_chioce": 5,
        # Part of url to get list of heavy proxies.
        "proxy_discovery_url": "hosts",
        # Timeout of proxy ban.
        "proxy_ban_timeout": 120 * 1000,

        # Link to operation in web interface.
        "operation_link_pattern": "http://{proxy}/#page=operation&mode=detail&id={id}&tab=details",

        # Sometimes proxy can return incorrect or incomplete response. This option enables checking response format for light requests.
        "check_response_format": True,
    },

    # This option enables logging on info level of all requests.
    "enable_request_logging": False,

    # This option allows to disable token.
    "enable_token": True,
    # If token specified than token_path ignored,
    # otherwise token extracted from file specified by token_path.
    "token": None,
    # $HOME/.yt/token by default
    "token_path": None,

    # Force using this version of api.
    "api_version": None,

    # Version of api for requests through http, None for use latest.
    # For native driver version "v3" by default.
    "default_api_version_for_http": "v3",

    # If this option disabled we use JSON for formatted requests
    # if format is not specified and yson_bindings is not installed.
    # It allows to build python structures much faster than using YSON.
    # But in this case we loose type of integer nodes.
    "force_using_yson_for_formatted_requests": False,

    # Driver configuration.
    "driver_config": None,

    # Enables generating request id and passing it to native driver.
    "enable_passing_request_id_to_driver": False,

    # Username for native driver requests.
    "driver_user_name": None,

    # Path to driver config.
    # ATTENTION: It is comptatible with native yt binary written in C++, it means
    # that config should be in YSON format and contain driver, logging and tracing configurations.
    # Do not use it for Yt client initialization, use driver_config instead.
    # Logging and tracing initialization would be executed only once for first initialization.
    "driver_config_path": None,

    # Path to file with additional configuration.
    "config_path": None,
    "config_format": "yson",

    "pickling": {
        # Extensions to consider while looking files to archive.
        "search_extensions": None,
        # Function to filter modules for archive.
        "module_filter": None,
        # Force using py-file even if pyc found.
        # It useful if local version of python differs from version installed on cluster.
        "force_using_py_instead_of_pyc": False,
        # Some package modules are created in .pth files or manually in hooks during import.
        # For example, .pth file could be used to emulate namespace packages (see PEP-420).
        # Such packages can lack of __init__.py and sometimes can not be imported on nodes
        # (e.g. because .pth files can not be taken to nodes)
        # In this case artificial __init__.py is added when modules achive is created.
        "create_init_file_for_package_modules": True,
        # The list of files to add into archive. File should be specified as tuple that
        # consists of absolute file path and relative path in archive.
        "additional_files_to_archive": None,
        # Function to replace standard py_wrapper.create_modules_archive.
        # If this function specified all previous options does not applied.
        "create_modules_archive_function": None,
        # Logging level of module finding errors.
        "find_module_file_error_logging_level": "WARNING",
        # Pickling framework used to save user modules.
        "framework": "dill",
        # Check that python version on local machine is the same as on cluster nodes.
        # Turn it off at your own risk.
        "check_python_version": False,
        # Path to python binary that would be used in jobs.
        "python_binary": "python",
        # Enable wrapping of stdin and stdout streams to avoid their unintentional usage.
        "safe_stream_mode": True,
        # Age (in seconds) to distinguish currently modified modules and old modules.
        # These two types of modules would be uploaded separatly.
        # It is invented to descrease data uploaded to cluster
        # in case of consequence runs of the script with small modifications.
        "fresh_files_threshold": 3600,
        # Enables using tmpfs for modules archive.
        "enable_tmpfs_archive": True,
        # Add tmpfs archive size to memory limit.
        "add_tmpfs_archive_size_to_memory_limit": True,
        # Enable collecting different statistics of job.
        "enable_job_statistics": True,
        # Should we assume that client and server are run on the same node.
        # Possible values:
        # False - all pickled data will be uploaded to cluster and then used in jobs.
        # True - we assumed that cluster run in local mode and the client code executed on the same node.
        # In this case jobs can use local files without uploading it on cluster.
        # None - client will try to auto-detect that server and client are run on the same node.
        "local_mode": None,
        # Collect dependencies for shared libraries automatically. All dependencies listed by
        # ldd command (and not filtered by "library_filter") will be added to special dir in
        # job sandbox and LD_LIBRARY_PATH will be set accordingly.
        "dynamic_libraries": {
            "enable_auto_collection": False,
            "library_filter": None
        }
    },

    # By default HTTP requests to YT are forbidden inside jobs to avoid strange errors
    # and unnecessary cluster accesses.
    "allow_http_requests_to_yt_from_job": False,

    "yamr_mode": {
        "always_set_executable_flag_on_files": False,
        "use_yamr_style_destination_fds": False,
        "treat_unexisting_as_empty": False,
        "delete_empty_tables": False,
        "use_yamr_sort_reduce_columns": False,
        "replace_tables_on_copy_and_move": False,
        "create_recursive": False,
        "throw_on_missing_destination": False,
        "run_map_reduce_if_source_is_not_sorted": False,
        "use_non_strict_upper_key": False,
        "check_input_fully_consumed": False,
        "abort_transactions_with_remove": False,
        "use_yamr_style_prefix": False,
        "create_tables_outside_of_transaction": False,

        # Special option that enables 4Gb memory_limit, 4Gb data_size_per_job and default zlib_6 codec for
        # newly created tables. These defaults are similar to defaults on Yamr-clusters, but look inappropriate
        # for YT. Please do not use this option in new code. This option will be deleted after
        # migration to YT from Yamr. More discussion can found in YT-5220.
        "use_yamr_defaults": False,

        # Enables ignoring empty tables in mapreduce -list command.
        "ignore_empty_tables_in_mapreduce_list": False,

        "check_codec_and_replication_factor": False,
    },

    # Run sorted merge instead of sort if input tables are sorted by sort_by prefix.
    "run_merge_instead_of_sort_if_input_tables_are_sorted": True,

    "tabular_data_format": None,

    # Attributes of automatically created tables.
    "create_table_attributes": None,

    # Remove temporary files after creation.
    "clear_local_temp_files": True,
    "local_temp_directory": "/tmp",

    # Path to remote directories for temporary files and tables.
    "remote_temp_files_directory": "//tmp/yt_wrapper/file_storage",
    "remote_temp_tables_directory": "//tmp/yt_wrapper/table_storage",

    "file_cache": {
        "replication_factor": 10,
    },

    "operation_tracker": {
        # Operation state check interval.
        "poll_period": 5000,
        # Log level used for print stderr messages.
        "stderr_logging_level": "INFO",
        # Log level used for printing operation progress.
        "progress_logging_level": "INFO",
        # Ignore failures during stderr downloads.
        "ignore_stderr_if_download_failed": False,
        # Abort operation when SIGINT is received while waiting for the operation to finish.
        "abort_on_sigint": True,
        # Log job statistics on operation complete.
        "log_job_statistics": False
    },

    # Size of block to read from response stream.
    "read_buffer_size": 8 * 1024 * 1024,

    # Defaults that will be passed to all operation specs
    "spec_defaults": {
    },
    "memory_limit": None,

    # Default value of table table writer configs.
    # It is passed to write_table and to job_io sections in operation specs.
    "table_writer": {
    },

    # TODO(ignat): rename to attached_operaion_mode = false
    # If detached False all operations run under special transaction. It causes operation abort if client died.
    "detached": True,

    # Prefix for all relative paths.
    "prefix": None,

    # Default timeout of transactions that started manually.
    "transaction_timeout": 15 * 1000,
    # How often wake up to determine whether transaction need to be pinged.
    "transaction_sleep_period": 100,
    # Use signal (SIGUSR1) instead of KeyboardInterrupt in main thread if ping failed.
    # Signal is sent to main thread and YtTransactionPingError is raised inside
    # signal handler. The error is processed inside __exit__ block: it will be thrown
    # out to user, all transactions in nested context managers will be aborted.
    # Be careful! If Transaction is created not in main thread this will cause
    # error "ValueError: signal only works in main thread".
    "transaction_use_signal_if_ping_failed": False,

    # Always write files as one chunks.
    # It forces disabling of write retries for large files.
    "write_file_as_one_chunk": False,

    # Default value of raw option in read, write, select, insert, lookup, delete.
    "default_value_of_raw_option": False,

    # Retries for read request. This type of retries parse data stream, if it is enabled, reading may be much slower.
    "read_retries": {
        "enable": True,
        "allow_multiple_ranges": False,
        "retry_count": 30,
        "chunk_unavailable_timeout": 60000,
        "create_transaction_and_take_snapshot_lock": True
    },

    # Retries for write commands. It split data stream into chunks and write it separately undef transactions.
    "write_retries": {
        "enable": True,
        # The size of data chunk that retried.
        # It is also used as a portion of reading file stream even if retries are disabled.
        "chunk_size": 512 * common.MB
    },

    # Retries for start operation requests.
    # It may fail due to violation of cluster operation limit.
    "start_operation_retries": {
        "retry_count": 30,
        "retry_timeout": 60000
    },

    "auto_merge_output": {
        # Action can be:
        # "none" - do nothing
        # "merge" - check output and merge chunks if necessary
        # "log" - check output and log result, do not merge
        "action": "log",
        "min_chunk_count": 1000,
        "max_chunk_size": 32 * common.MB
    },

    # Enable printing argcomplete errors.
    "argcomplete_verbose": False,

    # Enables using parse ypath that comes with yson bindigs for optimization.
    # Use this option with caution. YPath syntax in yson bindings and on cluster may differ.
    "enable_native_parse_ypath": False,

    # Do not fail with resolve error (just print warning) in yt.search(root, ...)
    # if root path does not exist.
    "ignore_root_path_resolve_error_in_search": False,

    "transform_options": {
        "chunk_count_to_compute_compression_ratio": 1,
        "desired_chunk_size": 2 * 1024 ** 3,
        "max_data_size_per_job": 16 * 1024 ** 3,
    }
}

def transform_value(value, original_value):
    if original_value is False or original_value is True:
        if isinstance(value, str):
            raise TypeError("Value must be boolean instead of string")
    if isinstance(value, YsonEntity):
        return None
    return value

def get_default_config():
    return VerifiedDict(["spec_defaults", "table_writer"], transform_value, deepcopy(default_config))
