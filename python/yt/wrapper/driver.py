"""YT requests misc"""
import config
import yt.logger as logger
from compression_wrapper import create_zlib_generator
from common import require, generate_uuid, bool_to_string, get_value, get_version
from errors import YtError, YtResponseError
from http import make_get_request_with_retries, make_request_with_retries, get_token, get_api, get_proxy_url, parse_error_from_headers

from yt.yson.convert import json_to_yson
import yt.yson as yson

import sys
import simplejson as json

def escape_utf8(obj):
    def escape_symbol(sym):
        if ord(sym) < 128:
            return sym
        else:
            return chr(ord('\xC0') | (ord(sym) >> 6)) + chr(ord('\x80') | (ord(sym) & ~ord('\xC0')))
    def escape_str(str):
        return "".join(map(escape_symbol, str))

    if isinstance(obj, unicode):
        obj = escape_str(str(bytearray(obj, 'utf-8')))
    elif isinstance(obj, str):
        obj = escape_str(obj)
    elif isinstance(obj, list):
        obj = map(escape_utf8, obj)
    elif isinstance(obj, dict):
        obj = dict((escape_str(k), escape_utf8(v)) for k, v in obj.iteritems())
    return obj

class ResponseStream(object):
    """Iterator over response"""
    def __init__(self, get_response, iter_content, close):
        self._buffer = ""
        self._pos = 0
        self._get_response = get_response
        self._iter_content = iter_content
        self._close = close

    @staticmethod
    def from_response(response):
        return ResponseStream(lambda: response, response.iter_content(config.READ_BUFFER_SIZE), lambda: response.close())

    def read(self, length=None):
        if length is None:
            length = 2 ** 32

        result = []
        if not self._buffer:
            self._fetch()

        while length > 0:
            right = min(len(self._buffer), self._pos + length)
            result.append(self._buffer[self._pos:right])
            length -= right - self._pos
            self._pos = right
            if length == 0 or not self._fetch():
                break
        return "".join(result)

    def readline(self):
        result = []
        while True:
            index = self._buffer.find("\n", self._pos)
            if index != -1:
                result.append(self._buffer[self._pos: index + 1])
                self._pos = index + 1
                break

            result.append(self._buffer[self._pos:])
            if not self._fetch():
                self._buffer = ""
                self._pos = 0
                break
        return "".join(result)

    def _fetch(self):
        def process_trailers():
            response = self._get_response()
            trailers = response.trailers()
            error = parse_error_from_headers(trailers)
            if error is not None:
                raise YtResponseError(response.url, response.headers, error)

        try:
            self._buffer = self._iter_content.next()
            self._pos = 0
            if not self._buffer:
                process_trailers()
                return False
            return True
        except StopIteration:
            process_trailers()
            return False

    def __iter__(self):
        return self

    def next(self):
        line = self.readline()
        if not line:
            raise StopIteration()
        return line

    def close(self):
        self._close()

def read_content(get_response, iter_content, raw, format=None):
    if raw:
        return ResponseStream(get_response, iter_content)
    else:
        return format.load_rows(ResponseStream(get_response, iter_content))


def get_hosts(client=None):
    proxy = get_proxy_url(None, client)
    hosts = None
    if client is not None:
        hosts = client.hosts
    if hosts is None:
        hosts = config.HOSTS

    return make_get_request_with_retries("http://{0}/{1}".format(proxy, hosts))

def get_heavy_proxy(client=None):
    client = get_value(client, config.CLIENT)
    if config.USE_HOSTS:
        hosts = get_hosts(client=client)
        if hosts:
            return hosts[0]
    if client is not None:
        return client.proxy
    else:
        return config.http.PROXY


def make_request(command_name, params,
                 data=None, proxy=None,
                 return_content=True, verbose=False,
                 retry_unavailable_proxy=True,
                 response_should_be_json=False,
                 use_heavy_proxy=False,
                 client=None):
    """
    Makes request to yt proxy. Command name is the name of command in YT API.
    """
    def print_info(msg, *args, **kwargs):
        # Verbose option is used for debugging because it is more
        # selective than logging
        if verbose:
            # We don't use kwargs because python doesn't support such kind of formatting
            print >>sys.stderr, msg % args
        logger.debug(msg, *args, **kwargs)

    # Prepare request url.
    if use_heavy_proxy:
        proxy = get_heavy_proxy(client)
    else:
        proxy = get_proxy_url(proxy, client)

    version, commands = get_api(client)
    api_path = "api/" + version

    # Get command description
    require(command_name in commands,
            YtError("Command {0} is not supported by {1}".format(command_name, api_path)))
    command = commands[command_name]

    # Determine make retries or not and set mutation if needed
    allow_retries = \
            not command.is_volatile or \
            (config.http.RETRY_VOLATILE_COMMANDS and not command.is_heavy)
    if config.RETRY is None:
        params["retry"] = bool_to_string(False)
    else:
        params["retry"] = bool_to_string(config.RETRY)
    if command.is_volatile and allow_retries:
        if config.MUTATION_ID is not None:
            params["mutation_id"] = config.MUTATION_ID
        else:
            params["mutation_id"] = generate_uuid()

    if config.TRACE is not None and config.TRACE:
        params["trace"] = bool_to_string(config.TRACE)

    # prepare url
    url = "http://{0}/{1}/{2}".format(proxy, api_path, command_name)
    print_info("Request url: %r", url)

    # prepare params, format and headers
    headers = {"User-Agent": "Python wrapper " + get_version(),
               "Accept-Encoding": config.http.ACCEPT_ENCODING,
               "X-YT-Correlation-Id": generate_uuid()}

    header_format = get_value(config.http.HEADER_FORMAT, "json")
    if header_format not in ["json", "yson"]:
        raise YtError("Incorrect headers format: " + str(header_format))
    def dumps(obj):
        if header_format == "json":
            return json.dumps(escape_utf8(yson.yson_to_json(obj)))
        if header_format == "yson":
            return yson.dumps(obj, yson_format="text")
        assert False

    header_format_header = header_format
    if header_format == "yson":
        header_format_header = "<format=text>yson"

    headers["X-YT-Header-Format"] = header_format_header
    if command.input_type is None:
        # Should we also check that command is volatile?
        require(data is None, YtError("Body should be empty in commands without input type"))
        if command.is_volatile:
            headers["Content-Type"] = "application/x-yt-yson-text" if header_format == "yson" else "application/json"
            data = dumps(params)
            params = {}

    if params:
        headers.update({"X-YT-Parameters": dumps(params)})

    if command.is_volatile and allow_retries:
        def set_retry():
            params["retry"] = bool_to_string(True)
            headers.update({"X-YT-Parameters": json.dumps(escape_utf8(params))})
        retry_action = set_retry
    else:
        retry_action = None

    token = get_token(client=client)
    if token is not None:
        headers["Authorization"] = "OAuth " + token

    if command.input_type in ["binary", "tabular"]:
        content_encoding = config.http.CONTENT_ENCODING
        headers["Content-Encoding"] = content_encoding
        if content_encoding == "identity":
            pass
        elif content_encoding == "gzip":
            data = create_zlib_generator(data)
        else:
            raise YtError("Content encoding '%s' is not supported" % config.http.CONTENT_ENCODING)

    # Debug information
    print_info("Headers: %r", headers)
    if command.input_type is None:
        print_info("Body: %r", data)

    stream = (command.output_type in ["binary", "tabular"])

    response = make_request_with_retries(
        command.http_method(),
        url,
        make_retries=allow_retries,
        retry_unavailable_proxy=retry_unavailable_proxy,
        retry_action=retry_action,
        headers=headers,
        data=data,
        stream=stream,
        response_should_be_json=response_should_be_json)

    print_info("Response headers %r", response.headers)

    # Determine type of response data and return it
    if return_content:
        return response.content
    else:
        return response

def make_formatted_request(command_name, params, format, **kwargs):
    # None format means that we want parsed output (as YSON structure) instead of string.
    # Yson parser is too slow, so we request result in JsonFormat and then convert it to YSON structure.

    if format is None:
        params["output_format"] = "json"
    else:
        params["output_format"] = format.to_yson_type()

    response_should_be_json = format is None
    result = make_request(command_name, params, response_should_be_json=response_should_be_json, **kwargs)

    if format is None:
        return json_to_yson(json.loads(result))
    else:
        return result
