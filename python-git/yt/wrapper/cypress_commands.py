import yson
from config import get_config, get_option
from common import parse_bool, flatten, get_value, bool_to_string, YtError
from errors import YtResponseError
from transaction_commands import _make_transactional_request, \
                                 _make_formatted_transactional_request
from table import prepare_path, to_name
from format import create_format

import yt.logger as logger

import os
import string
from copy import deepcopy

import __builtin__

def get(path, attributes=None, format=None, ignore_opaque=False, client=None):
    """Get Cypress node content (attribute tree).

    :param path: (string or `yt.wrapper.table.TablePath`) path to tree, it must exist!
    :param attributes: (list) desired node attributes in the response.
    :param format: (string or descendant of `yt.wrapper.format.Format`) output format \
        (by default python dict automatically parsed from YSON).
    :param ignore_opaque: (bool)
    Be careful: attributes have specific representation in JSON format.

    :return: node tree content in `format`
    .. seealso:: `get on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#get>`_
    """
    params = {
        "path": prepare_path(path, client=client),
        "ignore_opaque": bool_to_string(ignore_opaque)}
    if attributes is not None:
        params["attributes"] = attributes
    return _make_formatted_transactional_request(
        "get",
        params=params,
        format=format,
        client=client)

def set(path, value, format=None, client=None):
    """Set new value to Cypress node.

    :param path: (string or `yt.wrapper.table.TablePath`)
    :param value: json-able object.
    .. seealso:: `set on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#set>`_
    :param format: format of the value. If format is None than value should be json-able object. Otherwise it should be string.
    """
    if format is None:
        value = yson.dumps(value)
        format = "yson"

    if isinstance(format, str):
        format = create_format(format)

    return _make_transactional_request(
        "set",
        {
            "path": prepare_path(path, client=client),
            "input_format": format.to_yson_type()
        },
        data=value,
        client=client)

def copy(source_path, destination_path, recursive=None, preserve_account=None, force=None, client=None):
    """Copy Cypress node.

    :param source_path: (string or `yt.wrapper.table.TablePath`)
    :param destination_path: (string or `yt.wrapper.table.TablePath`)
    :param recursive: (bool) `config["yamr_mode"]["create_recursive"]` by default
    :param preserve_account: (bool)
    :param force: (bool)
    .. seealso:: `copy on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#copy>`_
    """
    params = {"source_path": prepare_path(source_path, client=client),
              "destination_path": prepare_path(destination_path, client=client)}

    recursive = get_value(recursive, get_config(client)["yamr_mode"]["create_recursive"])
    params["recursive"] = bool_to_string(recursive)

    if preserve_account is not None:
        params["preserve_account"] = bool_to_string(preserve_account)
    if force is not None:
        params["force"] = bool_to_string(force)
    return _make_transactional_request("copy", params, client=client)

def move(source_path, destination_path, recursive=None, preserve_account=None, force=None, client=None):
    """Move (rename) Cypress node.

    :param source_path: (string or `yt.wrapper.table.TablePath`)
    :param destination_path: (string or `yt.wrapper.table.TablePath`)
    :param recursive: (bool) `config["yamr_mode"]["create_recursive"]` by default
    :param preserve_account: (bool)
    :param force: (bool)
    .. seealso:: `move on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#move>`_
    """
    params = {"source_path": prepare_path(source_path, client=client),
              "destination_path": prepare_path(destination_path, client=client)}

    recursive = get_value(recursive, get_config(client)["yamr_mode"]["create_recursive"])
    params["recursive"] = bool_to_string(recursive)

    if preserve_account is not None:
        params["preserve_account"] = bool_to_string(preserve_account)
    if force is not None:
        params["force"] = bool_to_string(force)
    _make_transactional_request("move", params, client=client)

def concatenate(source_paths, destination_path, client=None):
    """Concatenate cypress nodes. This command applicable only to files and tables.

    :param source_path: (string or `yt.wrapper.table.TablePath`)
    :param destination_path: (string or `yt.wrapper.table.TablePath`)
    """
    source_paths = map(lambda path: prepare_path(path, client=client), source_paths)
    destination_path = prepare_path(destination_path, client=client)
    if not source_paths:
        raise YtError("Source paths must be non-empty")
    type = get(source_paths[0] + "/@type", client=client)
    if type not in ["file", "table"]:
        raise YtError("Type of '{0}' is not table or file".format(source_paths[0]))
    create(type, destination_path, ignore_existing=True)
    params = {"source_paths": source_paths,
              "destination_path": destination_path}
    _make_transactional_request("concatenate", params, client=client)

def link(target_path, link_path, recursive=False, ignore_existing=False, client=None):
    """Make link to Cypress node.

    :param target_path: (string or `yt.wrapper.table.TablePath`)
    :param link_path: (string or `yt.wrapper.table.TablePath`)
    :param recursive: (bool)
    :param ignore_existing: (bool)
    .. seealso:: `link on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#link>`_
    """
    return _make_transactional_request(
        "link",
        {
            "target_path": prepare_path(target_path, client=client),
            "link_path": prepare_path(link_path, client=client),
            "recursive": bool_to_string(recursive),
            "ignore_existing": bool_to_string(ignore_existing),
        },
        client=client)


def list(path, max_size=1000, format=None, absolute=False, attributes=None, client=None):
    """List directory (map_node) content.

    Node type must be 'map_node'.
    :param path: (string or `TablePath`)
    :param max_size: (int)
    :param attributes: (list) desired node attributes in the response.
    :param format: (descendant of `Format`) command response format, by default - YSON
    :param absolute: (bool) convert relative paths to absolute. Works only if format isn't specified.
    :return: raw YSON (string) by default, parsed YSON or JSON if format is specified.
    .. seealso:: `list on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#list>`_
    """
    def join(elem):
        return yson.to_yson_type(
            yson.YsonString("{0}/{1}".format(path, elem)),
            elem.attributes)

    params = {
        "path": prepare_path(path, client=client),
        "max_size": max_size}
    if attributes is not None:
        params["attributes"] = get_value(attributes, [])
    result = _make_formatted_transactional_request(
        "list",
        params=params,
        format=format,
        client=client)
    if absolute and format is None:
        result = map(join, result)
    return result

def exists(path, client=None):
    """Check Cypress node exists.

    :param path: (string or `TablePath`)
    .. seealso:: `exists on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#exists>`_
    """
    return parse_bool(
        _make_formatted_transactional_request(
            "exists",
            {"path": prepare_path(path, client=client)},
            format=None,
            client=client))

def remove(path, recursive=False, force=False, client=None):
    """Remove Cypress node.

    :param path: (string or `TablePath`)
    :param recursive: (bool)
    :param force: (bool)
    .. seealso:: `remove on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#remove>`_
    """
    _make_transactional_request(
        "remove",
        {
            "path": prepare_path(path, client=client),
            "recursive": bool_to_string(recursive),
            "force": bool_to_string(force)
        },
        client=client)

def create(type, path=None, recursive=False, ignore_existing=False, attributes=None, client=None):
    """Create Cypress node.

    :param type: (one of "table", "file", "map_node", "list_node"...) TODO(veronikaiv): list all types
    :param path: (string or `TablePath`)
    :param recursive: (bool) `config["yamr_mode"]["create_recursive"]` by default
    :param attributes: (dict)
    .. seealso:: `create on wiki <https://wiki.yandex-team.ru/yt/Design/ClientInterface/Core#create>`_
    """
    recursive = get_value(recursive, get_config(client)["yamr_mode"]["create_recursive"])
    params = {
        "type": type,
        "recursive": bool_to_string(recursive),
        "ignore_existing": bool_to_string(ignore_existing),
        "attributes": get_value(attributes, {})
    }
    if path is not None:
        params["path"] = prepare_path(path, client=client)
    return _make_formatted_transactional_request("create", params, format=None, client=client)

def mkdir(path, recursive=None, client=None):
    """Make directory (Cypress node of map_node type).
    :param path: (string or `TablePath`)
    :param recursive: (bool) `config["yamr_mode"]["create_recursive"]` by default
    """
    recursive = get_value(recursive, get_config(client)["yamr_mode"]["create_recursive"])
    return create("map_node", path, recursive=recursive, ignore_existing=recursive, client=client)

# TODO: maybe remove this methods
def get_attribute(path, attribute, default=None, client=None):
    """Get attribute of Cypress node.

    :param path: (string)
    :param attribute: (string)
    :param default: (any) return it if node hasn't attribute `attribute`."""
    if default is not None and attribute not in list_attributes(path, client=client):
        return default
    return get("%s/@%s" % (path, attribute), client=client)

def has_attribute(path, attribute, client=None):
    """Check Cypress node has attribute.

    :param path: (string)
    :param attribute: (string)
    """
    return exists("%s/@%s" % (path, attribute), client=client)

def set_attribute(path, attribute, value, client=None):
    """Set Cypress node `attribute` to `value`.

    :param path: (string)
    :param attribute: (string)
    :param value: (any)
    """
    return set("%s/@%s" % (path, attribute), value, client=client)

def list_attributes(path, attribute_path="", client=None):
    """List all attributes of Cypress node.

    :param path: (string)
    :param attribute_path: (string)
    """
    return list("%s/@%s" % (path, attribute_path), client=client)

def get_type(path, client=None):
    """Get Cypress node attribute type.

    :param path: (string)
    """
    return get_attribute(path, "type", client=client)

def find_free_subpath(path, client=None):
    """Generate some free random subpath.

    :param path: (string)
    :return: (string)
    """
    LENGTH = 10
    char_set = string.ascii_letters + string.digits
    while True:
        name = "".join([path] + get_option("_random_generator", client).sample(char_set, LENGTH))
        if not exists(name, client=client):
            return name

def search(root="", node_type=None,
           path_filter=None, object_filter=None, subtree_filter=None,
           map_node_order=None, list_node_order=None,
           attributes=None, exclude=None, depth_bound=None, client=None):
    """Search for some nodes in Cypress subtree.

    :param root: (string or `TablePath`) path to search
    :param node_type: (list of string)
    :param object_filter: (predicate)
    :param attributes: (list of string) these attributes will be added to result objects
    :param exclude: (list of string) excluded paths
    :param depth_bound: (int) recursion depth
    :return: (list of YsonString) result paths
    """
    # Deprecated. Default value "/" should be removed.
    if not root and not get_config(client)["prefix"]:
        root = "/"
    root = to_name(root, client=client)
    attributes = get_value(attributes, [])

    request_attributes = deepcopy(flatten(attributes))
    request_attributes.append("type")
    request_attributes.append("opaque")

    exclude = deepcopy(flatten(get_value(exclude, ["//sys/operations"])))

    def safe_get(path):
        try:
            return get(path, attributes=request_attributes, client=client)
        except YtResponseError as rsp:
            if rsp.is_access_denied():
                logger.warning("Cannot traverse %s, access denied" % path)
            elif rsp.is_resolve_error():
                logger.warning("Path %s is missing" % path)
            else:
                raise
        return None

    def is_opaque(object):
        # We have bug that get to document don't return attributes.
        return object.attributes.get("opaque", False) and object.attributes["type"] != "document"

    def walk(path, object, depth, ignore_opaque=False):
        if object is None:
            return
        if path in exclude or (depth_bound is not None and depth > depth_bound):
            return
        if is_opaque(object) and not ignore_opaque:
            for obj in walk(path, safe_get(path), depth, True):
                yield obj
            return
        if subtree_filter is not None and not subtree_filter(path, object):
            return

        object_type = object.attributes["type"]
        if (node_type is None or object_type in flatten(node_type)) and \
           (object_filter is None or object_filter(object)) and \
           (path_filter is None or path_filter(path)):
            yson_path = yson.YsonString(path)
            yson_path.attributes = dict(filter(lambda item: item[0] in attributes, object.attributes.iteritems()))
            yield yson_path

        if object_type in ["account_map", "tablet_cell"]:
            object = safe_get(path)

        if isinstance(object, dict):
            if map_node_order is not None:
                iteritems = ((key, object[key]) for key in map_node_order(path, object))
            else:
                iteritems = object.iteritems()
            for key, value in iteritems:
                for obj in walk("{0}/{1}".format(path, key), value, depth + 1):
                    yield obj

        if isinstance(object, __builtin__.list):
            if list_node_order is not None:
                enumeration = ((index, object[index]) for index in list_node_order(path, object))
            else:
                enumeration = enumerate(object)
            for index, value in enumeration:
                for obj in walk("{0}/{1}".format(path, index), value, depth + 1):
                    yield obj

    return walk(root, safe_get(root), 0, True)

def remove_with_empty_dirs(path, force=True, client=None):
    """Remove path and all empty dirs that appear after deletion.

    :param path: (string or `TablePath`)
    :param force: (bool)
    """
    path = to_name(path, client=client)
    while True:
        try:
            remove(path, recursive=True, force=True, client=client)
        except YtResponseError as error:
            if error.is_access_denied():
                logger.warning("Cannot remove %s, access denied", path)
                break
            else:
                raise
        path = os.path.dirname(path)
        if path == "//" or not exists(path, client=client) or list(path, client=client) or get(path + "/@acl", client=client):
            break

