from itertools import chain
from collections import Mapping
import os
import sys
import time
import ctypes
import types
import signal
import errno
import socket
from datetime import datetime

class YtError(Exception):
    """Base of all YT errors"""
    def __init__(self, message="", code=1, inner_errors=None, attributes=None):
        self.message = message
        self.code = code
        self.inner_errors = inner_errors if inner_errors is not None else []
        self.attributes = attributes if attributes else {}
        if "host" not in self.attributes:
            self.attributes["host"] = self._get_fqdn()
        if "datetime" not in self.attributes:
            self.attributes["datetime"] = datetime_to_string(datetime.now())

    def simplify(self):
        """ Transform error (with inner errors) to standard python dict """
        result = {"message": self.message, "code": self.code}
        if self.attributes:
            result["attributes"] = self.attributes
        if self.inner_errors:
            result["inner_errors"] = []
            for error in self.inner_errors:
                result["inner_errors"].append(
                    error.simplify() if isinstance(error, YtError) else
                    error)
        return result

    def __str__(self):
        return format_error(self)

    @staticmethod
    def _get_fqdn():
        if not hasattr(YtError, "cached_fqdn"):
            YtError._cached_fqdn = socket.getfqdn()
        return YtError._cached_fqdn

class YtResponseError(YtError):
    """Represents an error in YT response"""
    def __init__(self, error):
        super(YtResponseError, self).__init__(repr(error))
        self.error = error
        self.inner_errors = [self.error]

    def is_resolve_error(self):
        """Resolution error."""
        return self.contains_code(500)

    def is_access_denied(self):
        """Access denied."""
        return self.contains_code(901)

    def is_concurrent_transaction_lock_conflict(self):
        """Transaction lock conflict."""
        return self.contains_code(402)

    def is_request_rate_limit_exceeded(self):
        """Request rate limit exceeded."""
        return self.contains_code(904)

    def is_request_queue_size_limit_exceeded(self):
        """Request rate limit exceeded."""
        return self.contains_code(108)

    def is_chunk_unavailable(self):
        """Chunk unavailable."""
        return self.contains_code(716) or self.contains_text(" is unavailable")

    def is_request_timed_out(self):
        """Request timed out"""
        return self.contains_code(3)

    def is_concurrent_operations_limit_reached(self):
        """Too many concurrent operations"""
        return self.contains_code(202) or self.contains_text("Limit for the number of concurrent operations")

    def is_no_such_transaction(self):
        """No such transaction"""
        return self.contains_code(11000)

    def contains_code(self, code):
        """Check if HTTP response has specified error code."""
        def contains_code_recursive(error, error_code):
            if int(error.get("code", 0)) == error_code:
                return True
            for inner_error in error.get("inner_errors", []):
                if contains_code_recursive(inner_error, error_code):
                    return True
            return False

        return contains_code_recursive(self.error, code)

    def contains_text(self, text):
        """Check if HTTP response has specified status code."""
        def contains_text_recursive(error, text):
            message = ""
            if "message" in error:
                message = error["message"]

            if text in message:
                return True

            for inner_error in error.get("inner_errors", []):
                if contains_text_recursive(inner_error, text):
                    return True
            return False

        return contains_text_recursive(self.error, text)

def _pretty_format(error, attribute_length_limit=None, indent=0):
    def format_attribute(name, value):
        value = str(value)
        if attribute_length_limit is not None and len(value) > attribute_length_limit:
            value = value[:attribute_length_limit] + "...message truncated..."
        value = value.replace("\n", "\\n")
        return (" " * (indent + 4)) + "%-15s %s" % (name, value)

    if isinstance(error, YtError):
        error = error.simplify()
    elif isinstance(error, (Exception, KeyboardInterrupt)):
        error = {"code": 1, "message": str(error)}

    lines = []
    if "message" in error:
        lines.append(error["message"])

    if "code" in error and int(error["code"]) != 1:
        lines.append(format_attribute("code", error["code"]))

    attributes = error.get("attributes", {})

    origin_keys = ["host", "datetime"]
    origin_cpp_keys = ["pid", "tid", "fid"]
    if all(key in attributes for key in origin_keys):
        date = attributes["datetime"]
        if isinstance(date, datetime):
            date = date.strftime("%y-%m-%dT%H:%M:%S.%fZ")
        value = "{0} in {1}".format(attributes["host"], date)
        if all(key in attributes for key in origin_cpp_keys):
            value += "(pid %d, tid %x, fid %x)" % (attributes["pid"],attributes["tid"], attributes["fid"])
        lines.append(format_attribute("origin", value))

    location_keys = ["file", "line"]
    if all(key in attributes for key in location_keys):
        lines.append(format_attribute("location", "%s:%d" % (attributes["file"], attributes["line"])))

    for key, value in attributes.items():
        if key in origin_keys or key in location_keys or key in origin_cpp_keys:
            continue
        lines.append(format_attribute(key, value))

    result = " " * indent + (" " * (indent + 4) + "\n").join(lines)
    if "inner_errors" in error:
        for inner_error in error["inner_errors"]:
            # NB: here we should pass indent=indent + 2 as in C++ version, but historically there was a bug here.
            # We don't want fix it, because current format is enough pretty for users.
            result += "\n" + _pretty_format(inner_error, attribute_length_limit=attribute_length_limit, indent=indent)

    return result

def format_error(error, attribute_length_limit=300):
    return _pretty_format(error, attribute_length_limit)

def which(name, flags=os.X_OK):
    """ Return list of files in system paths with given name. """
    # TODO: check behavior when dealing with symlinks
    result = []
    for dir in os.environ.get("PATH", "").split(os.pathsep):
        path = os.path.join(dir, name)
        if os.access(path, flags):
            result.append(path)
    return result

def unlist(l):
    try:
        return l[0] if len(l) == 1 else l
    except TypeError: # cannot calculate len
        return l

def require(condition, exception_func):
    if not condition:
        raise exception_func()

def update(object, patch):
    if isinstance(patch, Mapping) and isinstance(object, Mapping):
        for key, value in patch.iteritems():
            if key in object:
                object[key] = update(object[key], value)
            else:
                object[key] = value
    elif isinstance(patch, types.ListType) and isinstance(object, types.ListType):
        for index, value in enumerate(patch):
            if index < len(object):
                object[index] = update(object[index], value)
            else:
                object.append(value)
    else:
        object = patch
    return object

def flatten(obj, list_types=(list, tuple, set, types.GeneratorType)):
    """ Create flat list from all elements. """
    if isinstance(obj, list_types):
        return list(chain(*map(flatten, obj)))
    return [obj]

def update_from_env(variables):
    """ Update variables dict from environment. """
    for key, value in os.environ.iteritems():
        prefix = "YT_"
        if not key.startswith(prefix):
            continue

        key = key[len(prefix):]
        if key not in variables:
            continue

        var_type = type(variables[key])
        # Using int we treat "0" as false, "1" as "true"
        if var_type == bool:
            try:
                value = int(value)
            except:
                pass
        # None type is treated as str
        if isinstance(None, var_type):
            var_type = str

        variables[key] = var_type(value)

def get_value(value, default):
    if value is None:
        return default
    else:
        return value

def filter_dict(predicate, dictionary):
    return dict([(k, v) for (k, v) in dictionary.iteritems() if predicate(k, v)])

def set_pdeathsig():
    if sys.platform.startswith("linux"):
        ctypes.cdll.LoadLibrary("libc.so.6")
        libc = ctypes.CDLL("libc.so.6")
        PR_SET_PDEATHSIG = 1
        libc.prctl(PR_SET_PDEATHSIG, signal.SIGTERM)

def remove_file(path, force=False):
    try:
        os.remove(path)
    except OSError:
        if not force:
            raise

def makedirp(path):
    try:
        os.makedirs(path)
    except OSError as err:
        if err.errno != errno.EEXIST:
            raise

def date_string_to_timestamp(date):
    # It is standard time representation in YT.
    return time.mktime(datetime.strptime(date, "%Y-%m-%dT%H:%M:%S.%fZ").timetuple())

def datetime_to_string(date):
    # It is standard time representation in YT.
    return date.strftime("%Y-%m-%dT%H:%M:%S.%fZ")

