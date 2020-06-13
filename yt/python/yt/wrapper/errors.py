"""YT usage errors"""

import yt.common
from yt.common import YtError, PrettyPrintableDict, get_value

from yt.packages.six import iteritems, text_type

from copy import deepcopy

def hide_fields(object, fields, hidden_value="hidden"):
    if isinstance(object, dict):
        for key in fields:
            if key in object:
                object[key] = hidden_value
        for key, value in iteritems(object):
            if isinstance(value, text_type) and value.startswith("AQAD-"):
                object[key] = hidden_value
            else:
                hide_fields(value, fields, hidden_value)

def hide_secure_vault(params):
    params = deepcopy(params)
    hide_fields(params, ("secure_vault",))
    return params

def hide_auth_headers(headers):
    headers = deepcopy(headers)
    if "Authorization" in headers:
        headers["Authorization"] = "x" * 32

    return headers

def hide_auth_headers_in_request_info(request_info):
    if "headers" in request_info:
        request_info = deepcopy(request_info)
        request_info["headers"] = hide_auth_headers(request_info["headers"])
    return request_info

class YtOperationFailedError(YtError):
    """Operation failed during waiting completion."""
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

        # TODO(ignat): Add all stderr as suberrors?
        if stderrs:
            failed_job = stderrs[0]
            failed_job_error = failed_job["error"]
            if "stderr" in failed_job:
                failed_job_error["attributes"]["stderr"] = failed_job["stderr"]
            inner_errors.append(failed_job_error)

        super(YtOperationFailedError, self).__init__(message, attributes=attributes, inner_errors=inner_errors)

class YtResponseError(yt.common.YtResponseError):
    """Another incarnation of YtResponseError."""
    def __init__(self, *args, **kwargs):
        super(YtResponseError, self).__init__(*args, **kwargs)
        if self.is_request_queue_size_limit_exceeded():
            self.__class__ = YtRequestQueueSizeLimitExceeded
        # Deprecated.
        elif self.contains_code(904):
            self.__class__ = YtRequestRateLimitExceeded
        elif self.is_concurrent_operations_limit_reached():
            self.__class__ = YtConcurrentOperationsLimitExceeded
        elif self.is_request_timed_out():
            self.__class__ = YtRequestTimedOut
        elif self.is_no_such_transaction():
            self.__class__ = YtNoSuchTransaction
        elif self.is_master_communication_error():
            self.__class__ = YtMasterCommunicationError
        elif self.is_chunk_unavailable():
            self.__class__ = YtChunkUnavailable
        elif self.is_concurrent_transaction_lock_conflict():
            self.__class__ = YtConcurrentTransactionLockConflict
        elif self.is_tablet_transaction_lock_conflict():
            self.__class__ = YtTabletTransactionLockConflict
        elif self.is_cypress_transaction_lock_conflict():
            self.__class__ = YtCypressTransactionLockConflict
        elif self.is_tablet_in_intermediate_state():
            self.__class__ = YtTabletIsInIntermediateState
        elif self.is_no_such_tablet():
            self.__class__ = YtNoSuchTablet
        elif self.is_tablet_not_mounted():
            self.__class__ = YtTabletNotMounted
        elif self.is_rpc_unavailable():
            self.__class__ = YtRpcUnavailable
        else:
            pass

class YtHttpResponseError(YtResponseError):
    """Reponse error recieved from http proxy with additional http request information."""
    def __init__(self, error, url, headers, params):
        super(YtHttpResponseError, self).__init__(error)
        self.url = url
        self.headers = deepcopy(headers)
        self.params = params
        self.message = "Received HTTP response with error"
        self.attributes.update({
            "url": url,
            "headers": PrettyPrintableDict(get_value(self.headers, {})),
            "params": PrettyPrintableDict(get_value(self.params, {})),
            "transparent": True})

    def __reduce__(self):
        return (YtHttpResponseError, (self.error, self.url, self.headers, self.params))

# TODO(ignat): All concrete errors below should be inherited from YtResponseError

class YtRequestRateLimitExceeded(YtHttpResponseError):
    """Request rate limit exceeded error.
       It is used in retries."""
    pass

class YtRequestQueueSizeLimitExceeded(YtHttpResponseError):
    """Request queue size limit exceeded error.
       It is used in retries.
    """
    pass

class YtRpcUnavailable(YtHttpResponseError):
    """Rpc unavailable error.
       It is used in retries.
    """
    pass

class YtConcurrentOperationsLimitExceeded(YtHttpResponseError):
    """Concurrent operations limit exceeded.
       It is used in retries."""
    pass

class YtRequestTimedOut(YtHttpResponseError):
    """Request timed out.
       It is used in retries."""
    pass

class YtNoSuchTransaction(YtHttpResponseError):
    """No such transaction.
       It is used in retries."""
    pass

class YtMasterCommunicationError(YtHttpResponseError):
    """Master communication error.
       It is used in retries."""
    pass

class YtChunkUnavailable(YtHttpResponseError):
    """Chunk unavalable error
       It is used in read retries"""
    pass

class YtCypressTransactionLockConflict(YtHttpResponseError):
    """Concurrent transaction lock conflict error.
       It is used in upload_file_to_cache retries."""
    pass

class YtTabletTransactionLockConflict(YtHttpResponseError):
    """Tablet transaction lock conflict error."""
    pass

# Deprecated.
YtConcurrentTransactionLockConflict = YtCypressTransactionLockConflict

class YtNoSuchService(YtHttpResponseError):
    """No such service error"""
    pass

class YtTabletIsInIntermediateState(YtHttpResponseError):
    """Tablet is in intermediate state error"""
    pass

class YtNoSuchTablet(YtHttpResponseError):
    """No such tablet error"""
    pass

class YtTabletNotMounted(YtHttpResponseError):
    """Tablet is not mounted error"""
    pass

class YtProxyUnavailable(YtError):
    """Proxy is under heavy load."""
    def __init__(self, response):
        self.response = response
        attributes = {
            "url": response.url,
            "request_info": hide_auth_headers_in_request_info(response.request_info)
        }
        super(YtProxyUnavailable, self).__init__(
            message="Proxy is unavailable",
            attributes=attributes,
            inner_errors=[response.error()])

class YtIncorrectResponse(YtError):
    """Incorrect proxy response."""
    def __init__(self, message, response):
        self.response = response
        attributes = {
            "url": response.url,
            "headers": response.headers,
            "request_info": hide_auth_headers_in_request_info(response.request_info),
            "body": self.truncate(response.text)}
        super(YtIncorrectResponse, self).__init__(message, attributes=attributes)

    def truncate(self, str):
        if len(str) > 100:
            return str[:100] + "...truncated"
        return str

class YtTokenError(YtError):
    """Some problem occurred with authentication token."""
    pass

class YtRetriableError(Exception):
    """Just simple retriable error for test purposes."""
    pass

class YtTransactionPingError(BaseException):
    """Raised in signal handler when thread was unable to ping transaction."""
    pass

class YtAllTargetNodesFailed(YtHttpResponseError):
    """Failed to write chunk since all target nodes have failed."""
    pass
