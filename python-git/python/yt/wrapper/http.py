import http_config
import logger
from common import require
from errors import YtError, YtResponseError, YtNetworkError, YtTokenError, YtProxyUnavailable, format_error

import os
import string
import time
import httplib
import requests
import simplejson as json

# We cannot use requests.HTTPError in module namespace because of conflict with python3 http library
from requests import HTTPError, ConnectionError, Timeout
NETWORK_ERRORS = (HTTPError, ConnectionError, Timeout, httplib.IncompleteRead, YtResponseError)

class Response(object):
    def __init__(self, http_response):
        self.http_response = http_response
        self._return_code_processed = False

    def error(self):
        self._process_return_code()
        return self._error

    def is_ok(self):
        self._process_return_code()
        return not hasattr(self, "_error")

    def is_json(self):
        return self.http_response.headers.get("content-type") == "application/json"

    def json(self):
        return self.http_response.json()

    def content(self):
        return self.http_response.content

    def _process_return_code(self):
        if self._return_code_processed:
            return

        if not str(self.http_response.status_code).startswith("2"):
            # 401 is case of incorrect token
            if self.http_response.status_code == 401:
                raise YtTokenError(
                    "Your authentication token was rejected by the server (X-YT-Request-ID: {0}).\n"
                    "Please refer to http://{1}/auth/ for obtaining a valid token or contact us at yt@yandex-team.ru."\
                        .format(
                            self.http_response.headers.get("X-YT-Request-ID", "absent"),
                            self.http_response.url))
            self._error = format_error(self.http_response.json())
        elif int(self.http_response.headers.get("x-yt-response-code", 0)) != 0:
            self._error = format_error(json.loads(self.http_response.headers["x-yt-error"]))
        self._return_code_processed = True

def make_request_with_retries(request, make_retries=False, retry_unavailable_proxy=True,
                              description="", return_raw_response=False):
    network_errors = list(NETWORK_ERRORS)
    if retry_unavailable_proxy:
        network_errors.append(YtProxyUnavailable)

    for attempt in xrange(http_config.HTTP_RETRIES_COUNT):
        try:
            response = request()
            # Sometimes (quite often) we obtain incomplete response with empty body where expected to be JSON.
            # So we should retry this request.
            is_json = response.is_json()
            if not return_raw_response and is_json and not response.content():
                raise YtResponseError(
                        "Response has empty body and JSON content type (Headers: %s)" %
                        repr(response.http_response.headers))
            if response.http_response.status_code == 503:
                raise YtProxyUnavailable("Retrying response with code 503 and body %s" % response.content())
            return response
        except tuple(network_errors) as error:
            message =  "HTTP request (%s) has failed with error '%s'" % (description, str(error))
            if make_retries:
                logger.warning("%s. Retrying...", message)
                time.sleep(http_config.HTTP_RETRY_TIMEOUT)
            elif not isinstance(error, YtError):
                # We wrapping network errors to simplify catching such errors later.
                raise YtNetworkError(message)
            else:
                raise

def make_get_request_with_retries(url):
    response = make_request_with_retries(
        lambda: Response(requests.get(url)),
        make_retries=True,
        description=url)
    return response.json()

def get_proxy(proxy):
    require(proxy, YtError("You should specify proxy"))
    return proxy

def get_api(proxy, version=None):
    location = "api" if version is None else "api/" + version
    return make_get_request_with_retries("http://{0}/{1}".format(get_proxy(proxy), location))

def get_token():
    token = http_config.TOKEN
    if token is None:
        token_path = os.path.join(os.path.expanduser("~"), ".yt/token")
        if os.path.isfile(token_path):
            token = open(token_path).read().strip()
    if token is not None:
        require(all(c in string.hexdigits for c in token),
                YtTokenError("You have an improper authentication token"))
    if not token:
        token = None
    return token

