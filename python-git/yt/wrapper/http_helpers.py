from .config import get_config, get_option, set_option, get_backend_type, get_request_retry_count
from .common import require, get_backoff, get_value, total_seconds, generate_uuid
from .errors import YtError, YtTokenError, YtProxyUnavailable, YtIncorrectResponse, YtHttpResponseError, \
                    YtRequestRateLimitExceeded, YtRequestQueueSizeLimitExceeded, YtRequestTimedOut, \
                    YtRetriableError, YtNoSuchTransaction, hide_token
from .command import parse_commands

import yt.logger as logger
import yt.yson as yson
import yt.json as json

from yt.packages.six import reraise, add_metaclass, PY3
from yt.packages.six.moves import xrange

import os
import sys
import random
import time
import types
import socket
from datetime import datetime
from socket import error as SocketError
from abc import ABCMeta, abstractmethod

# We cannot use requests.HTTPError in module namespace because of conflict with python3 http library
from yt.packages.six.moves.http_client import BadStatusLine, IncompleteRead

def get_retriable_errors():
    """List or errors that API will retry in HTTP requests."""
    from yt.packages.requests import HTTPError, ConnectionError, Timeout
    return (HTTPError, ConnectionError, Timeout, IncompleteRead, BadStatusLine, SocketError,
            YtIncorrectResponse, YtProxyUnavailable, YtRequestRateLimitExceeded, YtRequestQueueSizeLimitExceeded,
            YtRequestTimedOut, YtRetriableError)

@add_metaclass(ABCMeta)
class ProxyProvider(object):
    @abstractmethod
    def __call__(self):
        pass

    @abstractmethod
    def on_error_occured(self, error):
        pass

requests = None
def lazy_import_requests():
    global requests
    if requests is None:
        import yt.packages.requests
        requests = yt.packages.requests

def _get_session(client=None):
    lazy_import_requests()
    if get_option("_requests_session", client) is None:
        set_option("_requests_session", requests.Session(), client)
    return get_option("_requests_session", client)

def _cleanup_http_session(client=None):
    lazy_import_requests()
    set_option("_requests_session", requests.Session(), client)

def configure_ip(session, force_ipv4=False, force_ipv6=False):
    lazy_import_requests()
    if force_ipv4 or force_ipv6:
        import socket
        protocol = socket.AF_INET if force_ipv4 else socket.AF_INET6

        class HTTPAdapter(requests.adapters.HTTPAdapter):
            def init_poolmanager(self, *args, **kwargs):
                return super(HTTPAdapter, self).init_poolmanager(*args,
                                                                 socket_af=protocol,
                                                                 **kwargs)
        session.mount("http://", HTTPAdapter())

def parse_error_from_headers(headers):
    if int(headers.get("x-yt-response-code", 0)) != 0:
        return json.loads(headers["x-yt-error"])
    return None

def get_header_format(client):
    return get_value(get_config(client)["proxy"]["header_format"], "yson")

def check_response_is_decodable(response, format):
    if response.status_code // 100 != 2:
        return

    if format == "json":
        try:
            response.json()
        except (json.JSONDecodeError, TypeError):
            raise YtIncorrectResponse("Response body can not be decoded from JSON", response)
    elif format == "yson":
        try:
            yson.loads(response.content)
        except (yson.YsonError, TypeError):
            raise YtIncorrectResponse("Response body can not be decoded from YSON", response)


def create_response(response, request_info, client):
    def loads(str):
        header_format = get_header_format(client)
        if header_format == "json":
            return yson.json_to_yson(json.loads(str))
        if header_format == "yson":
            if PY3:
                # NOTE: Actually this is latin-1 encoding. urllib uses it to
                # decode headers, so it is used here to encode them back to bytes.
                str = str.encode("iso-8859-1")
            return yson.loads(str)
        raise YtError("Incorrect header format: {0}".format(header_format))

    def get_error():
        if not str(response.status_code).startswith("2"):
            check_response_is_decodable(response, format="json")
            return response.json()
        else:
            return parse_error_from_headers(response.headers)

    def error(self):
        return self._error

    def is_ok(self):
        return self._error is None

    if "X-YT-Response-Parameters" in response.headers:
        response.headers["X-YT-Response-Parameters"] = loads(response.headers["X-YT-Response-Parameters"])
    response.request_info = request_info
    response._error = get_error()
    response.error = types.MethodType(error, response)
    response.is_ok = types.MethodType(is_ok, response)
    return response

def _process_request_backoff(current_time, client):
    backoff = get_config(client)["proxy"]["request_backoff_time"]
    if backoff is not None:
        last_request_time = getattr(_get_session(client=client), "last_request_time", 0)
        now_seconds = total_seconds(current_time - datetime(1970, 1, 1))
        diff = now_seconds - last_request_time
        if diff * 1000.0 < float(backoff):
            time.sleep(float(backoff) / 1000.0 - diff)
        _get_session(client=client).last_request_time = now_seconds

def _raise_for_status(response, request_info):
    if response.status_code == 503:
        raise YtProxyUnavailable(response)
    if response.status_code == 401:
        url_base = "/".join(response.url.split("/")[:3])
        raise YtTokenError(
            "Your authentication token was rejected by the server (X-YT-Request-ID: {0}). "
            "Please refer to {1}/auth/ for obtaining a valid token if it will not fix error: "
            "please kindly submit a request to https://st.yandex-team.ru/createTicket?queue=YTADMINREQ"\
            .format(response.headers.get("X-YT-Request-ID", "missing"), url_base))

    if not response.is_ok():
        raise YtHttpResponseError(error=response.error(), **request_info)

def make_request_with_retries(method, url=None, make_retries=True, response_format=None,
                              params=None, timeout=None, retry_action=None, log_body=True,
                              is_ping=False, proxy_provider=None, client=None, **kwargs):
    """Performs HTTP request to YT proxy with retries."""
    configure_ip(_get_session(client),
                 get_config(client)["proxy"]["force_ipv4"],
                 get_config(client)["proxy"]["force_ipv6"])

    if timeout is None:
        timeout = get_config(client)["proxy"]["request_retry_timeout"] / 1000.0

    retriable_errors = list(get_retriable_errors())
    if is_ping:
        retriable_errors.append(YtNoSuchTransaction)

    headers = get_value(kwargs.get("headers", {}), {})
    headers["X-YT-Correlation-Id"] = generate_uuid(get_option("_random_generator", client))

    if proxy_provider is not None:
        url_pattern = url

    request_retry_count = get_request_retry_count(client)
    for attempt in xrange(request_retry_count):
        if proxy_provider is not None:
            proxy = proxy_provider()
            url = url_pattern.format(proxy=proxy)

        logger.debug("Request url: %r", url)
        logger.debug("Headers: %r", headers)
        if log_body and "data" in kwargs and kwargs["data"] is not None:
            logger.debug("Body: %r", kwargs["data"])

        request_start_time = datetime.now()
        _process_request_backoff(request_start_time, client=client)
        request_info = {"headers": headers, "url": url, "params": params}
        try:
            try:
                response = create_response(_get_session(client=client).request(method, url, timeout=timeout, **kwargs),
                                           request_info, client)

                if get_option("_ENABLE_HTTP_CHAOS_MONKEY", client) and random.randint(1, 5) == 1:
                    raise YtIncorrectResponse("", response)
            except requests.ConnectionError as error:
                # Module requests patched to process response from YT proxy
                # in case of large chunked-encoding write requests.
                # Here we check that this response was added to the error.
                if hasattr(error, "response"):
                    exc_info = sys.exc_info()
                    try:
                        # We should perform it under try..except due to response may be incomplete.
                        # See YT-4053.
                        rsp = create_response(error.response, request_info, client)
                    except:
                        reraise(*exc_info)
                    _raise_for_status(rsp, request_info)
                raise

            # Sometimes (quite often) we obtain incomplete response with body expected to be JSON.
            # So we should retry such requests.
            if response_format is not None and get_config(client)["proxy"]["check_response_format"]:
                check_response_is_decodable(response, response_format)

            logger.debug("Response headers %r", response.headers)

            _raise_for_status(response, request_info)
            return response
        except tuple(retriable_errors) as error:
            message = error.message if hasattr(error, "message") else str(error)
            logger.warning("HTTP %s request %s has failed with error %s, message: '%s', headers: %s",
                           method, url, str(type(error)), message, str(hide_token(dict(headers))))
            if isinstance(error, YtError):
                logger.info("Full error message:\n%s", str(error))
            if proxy_provider is not None:
                proxy_provider.on_error_occured(error)
            if make_retries and attempt + 1 < request_retry_count:
                if retry_action is not None:
                    retry_action(error, kwargs)
                backoff = get_backoff(
                    request_start_time=request_start_time,
                    request_timeout=get_config(client)["proxy"]["request_retry_timeout"],
                    is_request_heavy=False,
                    attempt=attempt,
                    backoff_config=get_config(client)["retry_backoff"])
                if backoff:
                    logger.warning("Sleep for %.2lf seconds before next retry", backoff)
                    time.sleep(backoff)
                logger.warning("New retry (%d) ...", attempt + 2)
            else:
                raise

    assert False, "Unknown error: this point should not be reachable"

def get_proxy_url(required=True, client=None):
    """Extracts proxy url from client and checks that url is specified."""
    proxy = get_config(client=client)["proxy"]["url"]

    if proxy is not None and "." not in proxy and "localhost" not in proxy and ":" not in proxy:
        proxy = proxy + get_config(client=client)["proxy"]["default_suffix"]

    if required:
        require(proxy, lambda: YtError("You should specify proxy"))

    return proxy

def _request_api(version=None, client=None):
    proxy = get_proxy_url(client=client)
    location = "api" if version is None else "api/" + version
    return make_request_with_retries("get", "http://{0}/{1}".format(proxy, location), client=client).json()

def get_api_version(client=None):
    api_version_option = get_option("_api_version", client)
    if api_version_option:
        return api_version_option

    api_version_from_config = get_config(client)["api_version"]
    if api_version_from_config:
        set_option("_api_version", api_version_from_config, client)
        return api_version_from_config


    if get_backend_type(client) == "http":
        default_api_version_for_http = get_config(client)["default_api_version_for_http"]
        if default_api_version_for_http is not None:
            api_version = default_api_version_for_http
        else:
            api_versions = _request_api(client=client)
            # To deprecate using api/v2
            api_versions.remove("v2")
            api_version = "v3"
            require(api_version in api_versions, lambda: YtError("API {0} is not supported".format(api_version)))
    else:
        api_version = "v3"

    set_option("_api_version", api_version, client)

    return api_version

def get_api_commands(client=None):
    if get_option("_commands", client):
        return get_option("_commands", client)

    commands = parse_commands(
        _request_api(
            version=get_api_version(client),
            client=client))
    set_option("_commands", commands, client)

    return commands

def get_fqdn(client=None):
    if get_option("_fqdn", client):
        return get_option("_fqdn", client)

    fqdn = socket.getfqdn()
    set_option("_fqdn", fqdn, client)

    return fqdn

def get_token(token=None, client=None):
    """Extracts token from given `token` and `client` arguments. Also checks token for correctness."""
    if token is None:
        if not get_config(client)["enable_token"]:
            return None

        token = get_config(client)["token"]
        if token is None:
            token_path = get_config(client=client)["token_path"]
            if token_path is None:
                token_path = os.path.join(os.path.expanduser("~"), ".yt/token")
            if os.path.isfile(token_path):
                with open(token_path, "r") as token_file:
                    token = token_file.read().strip()
                logger.debug("Token got from file %s", token_path)
        else:
            logger.debug("Token got from environment variable or config")

    # Empty token considered as missing.
    if not token:
        token = None

    # Validate token.
    if token is not None:
        require(all(33 <= ord(c) <= 126 for c in token),
                lambda: YtTokenError("You have an improper authentication token"))

    return token

def get_user_name(token=None, headers=None, client=None):
    """Requests auth method at proxy to receive user name by token or by cookies in header."""
    if get_backend_type(client) != "http":
        raise YtError("Function 'get_user_name' cannot be implemented for not http clients")

    if token is None and headers is None:
        token = get_token(client=client)

    version = get_api_version(client=client)
    proxy = get_proxy_url(client=client)

    if version == "v3":
        if headers is None:
            headers = {}
        if token is not None:
            headers["Authorization"] = "OAuth " + token.strip()
        data = None
        verb = "whoami"
    else:
        if not token:
            return None
        data = "token=" + token.strip()
        verb = "login"

    response = make_request_with_retries(
        "post",
        "http://{0}/auth/{1}".format(proxy, verb),
        headers=headers,
        data=data,
        client=client)
    login = response.json()["login"]
    if not login:
        return None
    return login
