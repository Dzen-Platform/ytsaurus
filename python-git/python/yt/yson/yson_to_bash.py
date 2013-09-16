#!/usr/bin/python
#!-*-coding:utf-8-*-

import parser

import sys
from optparse import OptionParser

# TODO: It is better to use class instead global variable
options = None

def require(condition, exception):
    if not condition:
        raise exception

def print_bash(obj, level):
    if not level:
        sys.stdout.write(options.sentinel)
        return

    scalar_types = [int, float, str]
    if obj is None:
        sys.stdout.write(options.none_literal)
    elif any(isinstance(obj, t) for t in scalar_types):
        sys.stdout.write(str(obj))
    elif isinstance(obj, list):
        sys.stdout.write(options.list_begin)
        first = True
        for item in obj:
            if not first:
                sys.stdout.write(options.list_separator)
            print_bash(item, level - 1)
            first = False
        sys.stdout.write(options.list_end)
    # TODO: extract list and dict processing to certain method
    elif isinstance(obj, dict):
        sys.stdout.write(options.map_begin)
        first = True
        for (key, value) in obj.iteritems():
            if not first:
                sys.stdout.write(options.map_separator)
            if not options.no_keys:
                print_bash(key, level - 1)
            if not options.no_keys and not options.no_values:
                sys.stdout.write(options.map_key_value_separator)
            if not options.no_values:
                print_bash(value, level - 1)
            first = False
        sys.stdout.write(options.map_end)
    else:
        # TODO: use here some YsonException instead of Exception
        raise Exception("Unknown type: %s" % type(obj))

def go_by_path(obj, path):
    # Is it dangerous to use here split?
    yson = obj
    path_elements = path.split("/")
    # TODO: add more information in Exceptions
    for elem in path_elements:
        if not elem: continue
        if isinstance(yson, list):
            require(elem.isdigit(), Exception("Incorrect path: list cannot be accessed by key '%s'" % elem))
            index = int(elem)
            require(0 <= index < len(yson), Exception("Incorrect path: list has no index %d" % index))
            yson = yson[index]
        elif isinstance(yson, dict):
            require(elem in yson, Exception("Incorrect path: map has no key '%s'" % elem))
            yson = yson[elem]
        else:
            raise Exception("Incorrect path: scalar cannot by accessed by key or index")
    return yson

if __name__ == "__main__":
    options_parser = OptionParser("Options")
    options_parser.add_option("--sentinel", default="")
    options_parser.add_option("--list_begin", default="")
    options_parser.add_option("--list_separator", default="\n")
    options_parser.add_option("--list_end", default="")
    options_parser.add_option("--none_literal", default="<None>")
    options_parser.add_option("--map_begin", default="")
    options_parser.add_option("--map_separator", default="\n")
    options_parser.add_option("--map_key_value_separator", default="\t")
    options_parser.add_option("--map_end", default="")
    options_parser.add_option("--print_depth", default=3, type=int)
    options_parser.add_option("--no_keys", default=False, action="store_const", const=True)
    options_parser.add_option("--no_values", default=False, action="store_const", const=True)

    options_parser.add_option("--path", default="")
    options, args = options_parser.parse_args()

    obj = go_by_path(parser.parse(sys.stdin), options.path)
    print_bash(obj, options.print_depth)
