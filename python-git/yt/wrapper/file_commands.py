"""downloading and uploading data to YT commands"""

import yt.logger as logger
from .config import get_config, get_option
from .common import require, chunk_iter_stream, chunk_iter_string, bool_to_string, parse_bool
from .errors import YtError, YtResponseError
from .http_helpers import get_api_version
from .heavy_commands import make_write_request, make_read_request
from .cypress_commands import remove, exists, set_attribute, mkdir, find_free_subpath, \
                              create, link, get, set
from .ypath import FilePath, ypath_join
from .local_mode import is_local_mode

from yt.yson import to_yson_type

import os
import hashlib

def _is_freshly_opened_file(stream):
    try:
        return hasattr(stream, "fileno") and stream.tell() == 0
    except IOError:
        return False

def _get_file_size(fstream):
    # We presuppose that current position in file is 0
    fstream.seek(0, os.SEEK_END)
    size = fstream.tell()
    fstream.seek(0, os.SEEK_SET)
    return size

# TODO(ignat): avoid copypaste (same function presented in py_wrapper.py)
def md5sum(filename):
    with open(filename, mode="rb") as fin:
        h = hashlib.md5()
        for buf in chunk_iter_stream(fin, 1024):
            h.update(buf)
    return h.hexdigest()

def read_file(path, file_reader=None, offset=None, length=None, client=None):
    """Download file from path in Cypress to local machine.

    :param path: (string of `FilePath`) path to file in Cypress
    :param response_type: (string) Deprecated! It means the output format. By default it is line generator.
    :param file_reader: (dict) spec of download command
    :param offset: (int) offset in input file in bytes, 0 by default
    :param length: (int) length in bytes of desired part of input file, all file without offset by default
    :return: some stream over downloaded file, string generator by default
    """
    path = FilePath(path, client=client)
    params = {"path": path}
    if file_reader is not None:
        params["file_reader"] = file_reader
    if length is not None:
        params["length"] = length
    if offset is not None:
        params["offset"] = offset

    def process_response(response):
        pass

    class RetriableState(object):
        def __init__(self):
            if offset is not None:
                self.offset = offset
            else:
                self.offset = 0
            self.length = length

        def prepare_params_for_retry(self):
            params["offset"] = self.offset
            if self.length is not None:
                params["length"] = self.length
            return params

        def iterate(self, response):
            for chunk in chunk_iter_stream(response, get_config(client)["read_buffer_size"]):
                if self.offset is not None:
                    self.offset += len(chunk)
                if self.length is not None:
                    self.length -= len(chunk)
                yield chunk

    command_name = "download" if get_api_version(client=client) == "v2" else "read_file"
    return make_read_request(
        command_name,
        path,
        params,
        process_response_action=process_response,
        retriable_state_class=RetriableState,
        client=client)

def download_file(path, file_reader=None, offset=None, length=None, client=None):
    """Download file from path in Cypress to local machine. Deprecated!
    .. seealso::  :py:func:`yt.wrapper.file_commands.read_file`.
    """
    return read_file(path=path, file_reader=file_reader,
                     offset=offset, length=length, client=client)

def write_file(destination, stream, file_writer=None, is_stream_compressed=False, force_create=None, client=None):
    """Upload file to destination path from stream on local machine.

    :param destination: (string or `FilePath`) destination path in Cypress
    :param stream: some stream, string generator or 'yt.wrapper.string_iter_io.StringIterIO' for example
    :param file_writer: (dict) spec of upload operation
    :param is_stream_compressed: (bool) expect stream to contain compressed data. \
    This data can be passed directly to proxy without recompression. Be careful! this option \
    disables write retries.
    :param force_create: (bool) unconditionally create file and ignores exsting file.
    """

    if force_create is None:
        force_create = True

    def prepare_file(path):
        if not force_create:
            return
        create("file", path, ignore_existing=True, client=client)

    chunk_size = get_config(client)["write_retries"]["chunk_size"]

    is_one_small_blob = False
    # Read out file into the memory if it is small.
    if _is_freshly_opened_file(stream) and _get_file_size(stream) <= chunk_size:
        stream = stream.read()
        is_one_small_blob = True

    # Read stream by chunks. Also it helps to correctly process StringIO from cStringIO (it has bug with default iteration).
    # Also it allows to avoid reading file by lines that may be slow.
    if hasattr(stream, "read"):
        # read files by chunks, not by lines
        stream = chunk_iter_stream(stream, chunk_size)
    if isinstance(stream, basestring):
        if isinstance(stream, unicode):
            stream = stream.encode("utf-8")
        if len(stream) <= chunk_size:
            is_one_small_blob = True
            stream = [stream]
        else:
            stream = chunk_iter_string(stream, chunk_size)

    params = {}
    if file_writer is not None:
        params["file_writer"] = file_writer

    enable_retries = get_config(client)["write_retries"]["enable"]
    if get_config(client)["write_file_as_one_chunk"]:
        if "file_writer" not in params:
            params["file_writer"] = {}
        params["file_writer"]["desired_chunk_size"] = 1024 ** 4
        if not is_one_small_blob:
            enable_retries = False

    if not is_one_small_blob and is_stream_compressed:
        enable_retries = False

    make_write_request(
        "upload" if get_api_version(client=client) == "v2" else "write_file",
        stream,
        destination,
        params,
        prepare_file,
        enable_retries,
        is_stream_compressed=is_stream_compressed,
        client=client)

def upload_file(stream, destination, file_writer=None, client=None):
    """Upload file to destination path from stream on local machine. Deprecated!
    .. seealso::  :py:func:`yt.wrapper.file_commands.write_file`.
    .. note:: upload_file and write_file have different argument order. \
    Be careful renaming upload_file to write_file!
    """
    write_file(destination=destination, stream=stream, file_writer=file_writer, client=client)

def is_executable(filename, client=None):
    return os.access(filename, os.X_OK) or get_config(client)["yamr_mode"]["always_set_executable_flag_on_files"]

def upload_file_to_cache(filename, hash=None, client=None):
    if hash is None:
        hash = md5sum(filename)

    last_two_digits_of_hash = ("0" + hash.split("-")[-1])[-2:]

    hash_path = ypath_join(get_config(client)["remote_temp_files_directory"], "hash")
    destination = ypath_join(hash_path, last_two_digits_of_hash, hash)

    attributes = None
    try:
        attributes = get(destination + "&/@", client=client)
    except YtResponseError as rsp:
        if not rsp.is_resolve_error():
            raise

    link_exists = False
    if attributes is not None:
        if attributes["type"] == "link":
            if parse_bool(attributes["broken"]):
                remove(destination + "&", client=client)
            else:
                link_exists = True
        else:
            remove(destination + "&", client=client)

    should_upload_file = not link_exists
    if link_exists:
        logger.debug("Link %s of file %s exists, skipping upload and set /@touched attribute", destination, filename)
        try:
            set(destination + "/@touched", True, client=client)
            set(destination + "&/@touched", True, client=client)
        except YtError as err:
            if err.is_resolve_error():
                should_upload_file = True
            elif not err.is_concurrent_transaction_lock_conflict():
                raise

    if should_upload_file:
        logger.debug("Link %s of file %s missing, uploading file", destination, filename)
        prefix = ypath_join(get_config(client)["remote_temp_files_directory"], last_two_digits_of_hash, os.path.basename(filename))
        # NB: In local mode we have only one node and default replication factor equal to one for all tables and files.
        if is_local_mode(client) or get_option("_is_testing_mode", client=client):
            replication_factor = 1
        else:
            if get_config(client)["file_cache"]["replication_factor"] < 3:
                raise YtError("File cache replication factor cannot be set less than 3")
            replication_factor = get_config(client)["file_cache"]["replication_factor"]
        real_destination = find_free_subpath(prefix, client=client)
        attributes = {
            "hash": hash,
            "touched": bool_to_string(True),
            "replication_factor": replication_factor
        }

        create("file",
               real_destination,
               recursive=True,
               attributes=attributes,
               client=client)
        write_file(real_destination, open(filename, "rb"), client=client)
        link(real_destination, destination, recursive=True, ignore_existing=True,
             attributes={"touched": bool_to_string(True)}, client=client)

    return destination

def smart_upload_file(filename, destination=None, yt_filename=None, placement_strategy=None,
                      ignore_set_attributes_error=True, hash=None, client=None):
    """
    Upload file to destination path with custom placement strategy.

    :param filename: (string) path to file on local machine
    :param destination: (string) desired file path in Cypress,
    :param yt_filename: (string) 'file_name' attribute of file in Cypress (visible in operation name of file), \
    by default basename of `destination` (or `filename` if `destination` is not set)
    :param placement_strategy: (one of "replace", "ignore", "random", "hash"), \
    "hash" by default.
    :param ignore_set_attributes_error: (bool) ignore `YtResponseError` during attributes setting
    :return: YSON structure with result destination path

    'placement_strategy':

    * "replace" or "ignore" -> destination path will be 'destination' \
    or 'config["remote_temp_files_directory"]/<basename>' if destination is not specified

    * "random" (only for None `destination` param) -> destination path will be 'config["remote_temp_files_directory"]/<basename><random_suffix>'\

    * "hash" (only for None `destination` param) -> destination path will be 'config["remote_temp_files_directory"]/hash/<md5sum_of_file>' \
    or this path will be link to some random Cypress path
    """

    def upload_with_check(path):
        require(not exists(path, client=client),
                lambda: YtError("Cannot upload file to '{0}', node already exists".format(path)))
        write_file(path, open(filename, "rb"), client=client)

    require(os.path.isfile(filename),
            lambda: YtError("Upload: %s should be file" % filename))

    if placement_strategy is None:
        placement_strategy = "hash"
    require(placement_strategy in ["replace", "ignore", "random", "hash"],
            lambda: YtError("Incorrect file placement strategy " + placement_strategy))

    if placement_strategy == "hash":
        if destination is not None:
            raise YtError("Option 'destination' can not be specified if strategy is 'hash'")
        if yt_filename is not None:
            raise YtError("Option 'yt_filename' can not be specified if strategy is 'hash'")
        yt_filename = os.path.basename(filename)
        destination = upload_file_to_cache(filename, hash, client=client)
    else:
        if destination is None:
            # create file storage dir and hash subdir
            mkdir(ypath_join(get_config(client)["remote_temp_files_directory"], "hash"), recursive=True, client=client)
            prefix = ypath_join(get_config(client)["remote_temp_files_directory"], os.path.basename(filename))
            destination = prefix
            if placement_strategy == "random":
                destination = find_free_subpath(prefix, client=client)
            if placement_strategy == "ignore" and exists(destination, client=client):
                return
            if yt_filename is None:
                yt_filename = os.path.basename(filename)
        else:
            if placement_strategy in ["hash", "random"]:
                raise YtError("Destination should not be specified if strategy is hash or random")
            mkdir(os.path.dirname(destination), recursive=True, client=client)
            if yt_filename is None:
                yt_filename = os.path.basename(destination)

        if placement_strategy == "replace":
            remove(destination, force=True, client=client)

        logger.debug("Uploading file '%s' with strategy '%s'", filename, placement_strategy)
        upload_with_check(destination)

    executable = os.access(filename, os.X_OK) or get_config(client)["yamr_mode"]["always_set_executable_flag_on_files"]

    try:
        set_attribute(destination, "file_name", yt_filename, client=client)
        set_attribute(destination, "executable", bool_to_string(executable), client=client)
    except YtResponseError as error:
        if error.is_concurrent_transaction_lock_conflict() and ignore_set_attributes_error:
            pass
        else:
            raise

    return to_yson_type(
        destination,
        {"file_name": yt_filename,
         "executable": bool_to_string(executable)})
