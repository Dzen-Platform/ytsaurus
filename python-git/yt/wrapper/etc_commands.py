from .driver import make_request, make_formatted_request
from .format import YsonFormat
from .common import set_param
from yt.ypath import parse_ypath

from yt.common import update
from yt.yson import loads, YsonString, YsonUnicode

import copy

def make_parse_ypath_request(path, client=None):
    attributes = {}
    if isinstance(path, (YsonString, YsonUnicode)):
        attributes = copy.deepcopy(path.attributes)

    result = loads(make_request(
        "parse_ypath",
        {"path": path, "output_format": YsonFormat(require_yson_bindings=False).to_yson_type()},
        client=client,
        decode_content=False))

    result.attributes = update(attributes, result.attributes)

    return result

def execute_batch(requests, concurrency=None, client=None):
    """Executes `requests` in parallel as one batch request."""
    params = {
        "requests": requests
    }
    set_param(params, "concurrency", concurrency)
    return make_formatted_request("execute_batch", params=params, format=None, client=client)

def dump_job_context(job_id, path, client=None):
    """Dumps job input context to specified path."""
    return make_request("dump_job_context", {"job_id": job_id, "path": path}, client=client)
