import collections
from itertools import chain
import types
import os

class YtError(Exception):
    """Base of all YT errors"""
    pass

def which(name, flags=os.X_OK):
    """ Return list of files in system paths with given name. """
    # TODO: check behavior when dealing with symlinks
    result = []
    for dir in os.environ.get("PATH", "").split(os.pathsep):
        path = os.path.join(dir, name)
        if os.access(path, flags):
            result.append(path)
    return result

def require(condition, exception):
    if not condition: raise exception

def update(d, u):
    for k, v in u.iteritems():
        if isinstance(v, collections.Mapping):
            r = update(d.get(k, {}), v)
            d[k] = r
        else:
            d[k] = u[k]
    return d

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

