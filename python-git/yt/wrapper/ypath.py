from .etc_commands import parse_ypath
from .common import flatten, bool_to_string, parse_bool, update, require
from .errors import YtError
from .config import get_config

import yt.yson as yson
from yt.packages.six import iteritems
from yt.packages.six.moves import map as imap

from copy import deepcopy
import string

def ypath_join(*paths):
    """ Join parts of cypress paths. """
    def ends_with_slash(part):
        if part.endswith("/"):
            if part.endswith("\\/"):
                raise YtError("Path with \\\\/ found, failed to join it")
            return True
        return False

    result = []
    for path in paths:
        if path.startswith("//") or path == "/":
            result = []

        slash_count = 0
        if path != "/":
            if path.startswith("/"):
                slash_count += 1
            if result and ends_with_slash(result[-1]):
                slash_count += 1

        if slash_count == 2:
            result.append(path[1:])
        else: # slash_count <= 1
            if (slash_count == 0 and result) or result == ["/"]:
                result.append("/")
            result.append(path)

    return "".join(result)

def escape_ypath_literal(literal):
    """ Escapes string to use it as key in ypath. """
    def escape_char(ch):
        if ch in ["\\", "/", "@", "&", "[", "{"]:
            return "\\" + ch
        num = ord(ch)
        if num >= 256:
            raise YtError("YPath literals should consist of bytes with code in [0, 255]")
        if num < 32: # or num >= 128:
            return "\\x" + string.hexdigits[num // 16] + string.hexdigits[num % 16]
        return ch

    return "".join(imap(escape_char, literal))

# XXX(ignat): Inherit from YsonString?
class YPath(object):
    """
    Represents YPath with attributes.

    Options:

    * path -- string representing cypress path, possible with YPath-encoded attributes

    * attributes -- additinal attributes

    * simplify -- perform parsing of given path

    .. seealso:: `YPath on wiki <https://wiki.yandex-team.ru/yt/userdoc/ypath>`_
    """
    def __init__(self,
                 path,
                 simplify=True,
                 attributes=None,
                 client=None):

        if isinstance(path, YPath):
            self._path_object = deepcopy(path._path_object)
        else:
            if simplify:
                self._path_object = parse_ypath(path, client=client)
                for key, value in iteritems(self._path_object.attributes):
                    if "-" in key:
                        self._path_object.attributes[key.replace("-", "_")] = value
                        del self._path_object.attributes[key]
            else:
                self._path_object = yson.to_yson_type(path)

        if str(self._path_object) != "/" and not self._path_object.startswith("//") and not self._path_object.startswith("#"):
            prefix = get_config(client)["prefix"]
            require(prefix,
                    lambda: YtError("Path '%s' should be absolute or you should specify a prefix" % self._path_object))
            require(prefix.startswith("//"),
                    lambda: YtError("PREFIX '%s' should start with //" % prefix))
            require(prefix.endswith("/"),
                    lambda: YtError("PREFIX '%s' should end with /" % prefix))
            # TODO(ignat): refactor YsonString to fix this hack
            copy_attributes = self._path_object.attributes
            self._path_object = yson.to_yson_type(prefix + self._path_object if self._path_object else prefix[:-1])
            self._path_object.attributes = copy_attributes

        if attributes is not None:
            self._path_object.attributes = update(self._path_object.attributes, attributes)

    @property
    def attributes(self):
        return self._path_object.attributes

    def to_yson_type(self):
        """Return YSON representation of path."""
        return self._path_object

    def to_yson_string(self):
        """Return yson path with attributes as string."""
        if self.attributes:
            attributes_str = yson._dumps_to_native_str(self.attributes, yson_type="map_fragment", yson_format="text")
            # NB: in text format \n can appear only as separator.
            return "<{0}>{1}".format(attributes_str.replace("\n", ""), str(self._path_object))
        else:
            return str(self._path_object)

    def join(self, other):
        """Join ypath with other path."""
        return YPath(ypath_join(str(self), other), simplify=False)

    def __eq__(self, other):
        # TODO(ignat): Fix it, compare with attributes!
        if isinstance(other, YPath):
            return str(self._path_object) == str(other._path_object)
        else:
            return str(self._path_object) == other

    def __ne__(self, other):
        return not (self == other)

    def __hash__(self):
        return hash(self._path_object)

    def __str__(self):
        return str(self._path_object)

    def __repr__(self):
        return self.to_yson_string()

    def __add__(self, other):
        return YPath(str(self) + other, simplify=False)

class YPathSupportingAppend(YPath):
    def __init__(self, path, simplify=True, attributes=None, append=None, client=None):
        super(YPathSupportingAppend, self).__init__(path, simplify=simplify, attributes=attributes, client=client)
        self._append = None
        if append is not None:
            self.append = append
        elif "append" in self.attributes:
            self.append = self.attributes["append"]

    @property
    def append(self):
        if self._append is not None:
            return parse_bool(self._append)
        else:
            return None

    @append.setter
    def append(self, value):
        self._append = value
        if self._append is not None:
            self.attributes["append"] = bool_to_string(self._append)
        else:
            if "append" in self.attributes:
                del self.attributes["append"]

def to_ypath(object, client=None):
    if isinstance(object, YPath):
        return object
    else:
        return YPath(object, client=client)

class TablePath(YPathSupportingAppend):
    """
    Table ypath.

    Supported attributes:

    * append -- append to table or overwrite

    * columns -- list of string (column) or string pairs (column range).

    * exact_key, lower_key, upper_key -- tuple of strings to identify range of rows

    * exact_index, start_index, end_index -- tuple of indexes to identify range of rows

    .. seealso:: `YPath on wiki <https://wiki.yandex-team.ru/yt/userdoc/ypath>`_
    """
    def __init__(self,
                 # TODO(ignat): rename to path
                 name,
                 append=None,
                 sorted_by=None,
                 columns=None,
                 exact_key=None,
                 lower_key=None,
                 upper_key=None,
                 exact_index=None,
                 start_index=None,
                 end_index=None,
                 ranges=None,
                 schema=None,
                 simplify=True,
                 attributes=None,
                 client=None):
        """
        :param name: (Yson string) path with attribute
        :param append: (bool) append to table or overwrite
        :param sorted_by: (list of string) list of sort keys
        :param columns: list of string (column) or string pairs (column range)
        :param exact_key: (string or string tuple) exact key of row
        :param lower_key: (string or string tuple) lower key bound of rows
        :param upper_key: (string or string tuple) upper bound of rows
        :param exact_index: (int) exact index of row
        :param start_index: (int) lower bound of rows
        :param end_index: (int) upper bound of rows
        :param ranges: (list) list of ranges of rows. It overwrites all other row limits.
        :param schema: (list) table schema description.
        :param attributes: (dict) attributes, it updates attributes specified in name.

        `See usage example. <https://wiki.yandex-team.ru/yt/userdoc/ypath/#raspoznavaemyesistemojjatributy>`_
        .. note:: don't specify lower_key (upper_key) and start_index (end_index) simultaneously
        """

        super(TablePath, self).__init__(name, simplify=simplify, attributes=attributes, append=append, client=client)

        attributes = self._path_object.attributes
        if "channel" in attributes:
            attributes["columns"] = attributes["channel"]
            del attributes["channel"]
        if sorted_by is not None:
            attributes["sorted_by"] = sorted_by
        if columns is not None:
            attributes["columns"] = columns
        if schema is not None:
            attributes["schema"] = schema


        if ranges is not None:
            def _check_option(value, option_name):
                if value is not None:
                    raise YtError("Option '{0}' cannot be specified with 'ranges' option".format(option_name))

            for value, name in [(exact_key, "exact_key"), (exact_index, "exact_index"),
                                (lower_key, "lower_key"), (start_index, "start_index"),
                                (upper_key, "upper_key"), (end_index, "end_index")]:
                _check_option(value, name)

            attributes["ranges"] = ranges

        else:
            if start_index is not None and lower_key is not None:
                raise YtError("You could not specify lower key bound and start index simultaneously")
            if end_index is not None and upper_key is not None:
                raise YtError("You could not specify upper key bound and end index simultaneously")

            range = {}
            if "exact" in attributes:
                range["exact"] = attributes["exact"]
                del attributes["exact"]
            if "lower_limit" in attributes:
                range["lower_limit"] = attributes["lower_limit"]
                del attributes["lower_limit"]
            if "upper_limit" in attributes:
                range["upper_limit"] = attributes["upper_limit"]
                del attributes["upper_limit"]

            if exact_key is not None:
                range["exact"] = {"key": flatten(exact_key)}
            if lower_key is not None:
                range["lower_limit"] = {"key": flatten(lower_key)}
            if upper_key is not None:
                if get_config(client)["yamr_mode"]["use_non_strict_upper_key"]:
                    upper_key = upper_key + "\0"
                range["upper_limit"] = {"key": flatten(upper_key)}
            if exact_index is not None:
                range["exact"] = {"row_index": exact_index}
            if start_index is not None:
                range["lower_limit"] = {"row_index": start_index}
            if end_index is not None:
                range["upper_limit"] = {"row_index": end_index}

            if range:
                attributes["ranges"] = [range]

    def has_delimiters(self):
        """Check attributes for delimiters (channel, lower or upper limits)."""
        return any(key in self.attributes for key in ["columns", "lower_limit", "upper_limit", "ranges"])

class FilePath(YPathSupportingAppend):
    """
    File ypath.

    Supported attributes:

    * file_ -- append to table or overwrite

    * columns -- list of string (column) or string pairs (column range).
    """
    def __init__(self, path, append=None, executable=None, file_name=None, simplify=None, attributes=None, client=None):
        super(FilePath, self).__init__(path, attributes=attributes, simplify=simplify, append=append, client=client)
        if executable is not None:
            self.attributes["executable"] = executable
        if file_name is not None:
            self.attributes["file_name"] = file_name
