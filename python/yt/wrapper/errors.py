"""YT usage errors"""

from yt.common import YtError

from copy import deepcopy
import simplejson as json

def hide_token(headers):
    if "Authorization" in headers:
        headers["Authorization"] = "x" * 32
    return headers

class YtOperationFailedError(YtError):
    """Operation failed during WaitStrategy.process_operation."""
    def __init__(self, id, state, error, stderrs, url):
        message = "Operation {0} {1}".format(id, state)
        attributes = {
            "id": id,
            "state": state,
            "stderrs": stderrs,
            "url": url}

        inner_errors = []
        if error is not None:
            inner_errors.append(error)

        super(YtOperationFailedError, self).__init__(message, attributes=attributes, inner_errors=inner_errors)

class YtTimeoutError(YtError):
    """WaitStrategy timeout expired."""
    pass

class YtResponseError(YtError):
    """Error in HTTP response."""
    def __init__(self, error):
        super(YtResponseError, self).__init__(repr(error))
        self.error = error
        self.inner_errors = [self.error]

    def is_resolve_error(self):
        """Resolving error."""
        return YtResponseError._contains_code(self.error, 500)

    def is_access_denied(self):
        """Access denied."""
        return YtResponseError._contains_code(self.error, 901)

    def is_concurrent_transaction_lock_conflict(self):
        """Transaction lock conflict."""
        return YtResponseError._contains_code(self.error, 402)

    def is_request_rate_limit_exceeded(self):
        """Request rate limit exceeded."""
        return YtResponseError._contains_code(self.error, 904)

    @staticmethod
    def _contains_code(error, code):
        if int(error["code"]) == code:
            return True
        for inner_error in error["inner_errors"]:
            if YtResponseError._contains_code(inner_error, code):
                return True
        return False

class YtHttpResponseError(YtResponseError):
    def __init__(self, url, headers, error):
        super(YtHttpResponseError, self).__init__(error)
        self.url = url
        self.headers = deepcopy(headers)
        self.message = "Received an error while requesting {0}. Request headers are {1}"\
            .format(url, json.dumps(hide_token(self.headers), indent=4, sort_keys=True))


class YtRequestRateLimitExceeded(YtHttpResponseError):
    pass

def build_http_response_error(url, headers, error):
    if YtHttpResponseError._contains_code(error, 904):
        return YtRequestRateLimitExceeded(url, headers, error)
    return YtHttpResponseError(url, headers, error)

class YtProxyUnavailable(YtError):
    """Proxy is under heavy load."""
    pass

class YtIncorrectResponse(YtError):
    """Incorrect proxy response."""
    pass

class YtTokenError(YtError):
    """Some problem occurred with authentication token."""
    pass


