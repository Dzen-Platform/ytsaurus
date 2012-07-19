from common import add_quotes
from http import make_request

import os
from itertools import imap, izip

def get(path, check_errors=True):
    return make_request("GET", "get", dict(path=path), check_errors=check_errors)

def set(path, value):
    return make_request("PUT", "set", dict(path=path), value)

def copy(source_path, destination_path):
    return make_request("GET", "copy", locals())

def list(path, check_existance=True, quoted=True):
    if check_existance and not exists(path):
        return []
    result = make_request("GET", "list", {"path": path})
    if quoted:
        result = imap(add_quotes, result)
    return result

def exists(path, hint=""):
    # TODO(ignat): use here not already existed function 'exists' from http
    objects = get("/")
    if path == "/": return True
    cur_path = "/"
    for elem in path.strip("/").split("/"):
        elem = elem.strip('"')
        if objects is None:
            objects = get(cur_path)
        if not isinstance(objects, dict) or elem not in objects:
            return False
        else:
            objects = objects[elem]
            cur_path = '%s/"%s"' % (cur_path, elem)
    return True

def remove(path):
    if exists(path):
        return make_request("POST", "remove", {"path": path})
    return None

def get_attribute(path, attribute, check_errors=True):
    return get("%s/@%s" % (path, attribute), check_errors=check_errors)

def set_attribute(path, attribute, value):
    return set("%s/@%s" % (path, attribute), value)

def list_attributes(path):
    return list(path + "/@")

