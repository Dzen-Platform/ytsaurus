#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # noqa

from prepare_source_tree_helpers import (
    apply_multiple,
    cp_r,
    logger,
    replace,
    replace_symlink,
    rm_rf,
)

import argparse
import glob


YT_PYTHON_PACKAGE_LIST = [
    "certifi",
    "dill",
    "requests",
    "distro",
    ("urllib3", "urllib3/src"),
]

CONTRIB_PYTHON_PACKAGE_LIST = [
    ("argcomplete", "argcomplete/py2"),
    "simplejson",
    "cloudpickle",
    "decorator",
    "backports_abc",
    "singledispatch",
    ("tornado", "tornado/tornado-4"),
    "tqdm",
    "chardet",
    ("idna", "idna/py2"),
    "six",
    ("attr", "attrs"),
]

LIBRARY_PYTHON_PACKAGE_LIST = [
    "type_info",
]

PY23_BINARIES = [
    "yt/wrapper/bin/yt",
    "yt/wrapper/bin/yt-fuse",
    "yt/wrapper/bin/yt-admin",
    "yt/wrapper/bin/yt-job-tool",
    "yt/wrapper/bin/mapreduce-yt",
]

YT_PREFIX_BINARIES = [
    "yt/tools/bin/add_user.py",
    "yt/tools/bin/checksum.py",
    "yt/tools/bin/lock.py",
    "yt/tools/bin/set_account.py",
]


def fix_chardet_package(chardet_path):
    for root, dirs, files in os.walk(chardet_path):
        for file in files:
            with open(os.path.join(root, file)) as fin:
                data = fin.read()
            data = data.replace(
                "from chardet.sbcharsetprober import SingleByteCharSetModel\n",
                "from .sbcharsetprober import SingleByteCharSetModel\n")
            with open(os.path.join(root, file), "w") as fout:
                fout.write(data)


def fix_type_info_package(type_info_path):
    for root, dirs, files in os.walk(type_info_path):
        for file in files:
            with open(os.path.join(root, file)) as fin:
                data = fin.read()
            data = data.replace(
                "import six\n",
                "import yt.packages.six as six\n")
            with open(os.path.join(root, file), "w") as fout:
                fout.write(data)


def prepare_python_source_tree(python_root, yt_root, arcadia_root=None,
                               prepare_binary_symlinks=False, prepare_bindings=True):
    def python_contrib_path(path):
        return os.path.join(python_root, "contrib", path)

    if arcadia_root is None:
        arcadia_root = yt_root

    packages_dir = os.path.join(python_root, "yt", "packages")
    library_dir = os.path.join(python_root, "yandex")

    # Cleanup old stuff
    for fname in os.listdir(packages_dir):
        for package in YT_PYTHON_PACKAGE_LIST + CONTRIB_PYTHON_PACKAGE_LIST:
            if fname.startswith(package):
                rm_rf(os.path.join(packages_dir, fname))

    if os.path.exists(library_dir):
        rm_rf(library_dir)

    # Prepare library dependencies
    if not os.path.exists(library_dir):
        os.makedirs(library_dir)
    open(os.path.join(library_dir, "__init__.py"), "a").close()

    for package_name in LIBRARY_PYTHON_PACKAGE_LIST:
        logger.info("Preparing package %s", package_name)

        path_to_copy = "{arcadia_root}/library/python/{package_name}".format(
            arcadia_root=arcadia_root,
            package_name=package_name)

        cp_r(path_to_copy, library_dir)

        if package_name == "type_info":
            fix_type_info_package(os.path.join(library_dir, package_name))

    # Prepare contribs
    for package_name in YT_PYTHON_PACKAGE_LIST:
        if isinstance(package_name, tuple):
            package_name, package_relative_path = package_name
        else:
            package_relative_path = package_name

        logger.info("Preparing package %s", package_name)
        files_to_copy = glob.glob(
            "{python_root}/contrib/python-{package_relative_path}/{package_name}*.py".format(
                python_root=python_root,
                package_relative_path=package_relative_path,
                package_name=package_name))
        files_to_copy += glob.glob(
            "{python_root}/contrib/python-{package_relative_path}/{package_name}".format(
                python_root=python_root,
                package_relative_path=package_relative_path,
                package_name=package_name))
        for path in files_to_copy:
            cp_r(path, packages_dir)

    for package_name in CONTRIB_PYTHON_PACKAGE_LIST:
        if isinstance(package_name, tuple):
            package_name, package_relative_path = package_name
        else:
            package_relative_path = package_name

        logger.info("Preparing package %s", package_name)
        # By some reason tqdm has both tqdm dir and tqdm.py file, second must be ignored.
        files_to_copy = []
        if package_name != "tqdm":
            files_to_copy += glob.glob(
                "{arcadia_root}/contrib/python/{package_name}/{package_name}*.py".format(
                    arcadia_root=arcadia_root,
                    package_name=package_name))
        files_to_copy += glob.glob(
            "{arcadia_root}/contrib/python/{package_relative_path}/{package_name}".format(
                arcadia_root=arcadia_root,
                package_relative_path=package_relative_path,
                package_name=package_name))
        for path in files_to_copy:
            cp_r(path, packages_dir)

        if package_name == "chardet":
            fix_chardet_package(os.path.join(packages_dir, "chardet"))

    # Replace certificate.
    cp_r(os.path.join(arcadia_root, "certs", "cacert.pem"), os.path.join(packages_dir, "certifi"))

    replace(python_contrib_path("python-fusepy/fuse.py"), packages_dir)

    if prepare_bindings:
        replace(os.path.join(yt_root, "yt/python/yt_yson_bindings"), python_root)

        yt_driver_bindings = os.path.join(yt_root, "yt/python/yt_driver_bindings")
        if os.path.exists(yt_driver_bindings):
            replace(yt_driver_bindings, python_root)

        yt_driver_rpc_bindings = os.path.join(yt_root, "yt/python/yt_driver_rpc_bindings")
        if os.path.exists(yt_driver_rpc_bindings):
            replace(yt_driver_rpc_bindings, python_root)

    if prepare_binary_symlinks:
        for binary in PY23_BINARIES:
            binary_path = os.path.join(python_root, binary)
            for suffix in ("2", "3"):
                replace_symlink(binary_path, binary_path + suffix)

        for binary in YT_PREFIX_BINARIES:
            binary_path = os.path.join(python_root, binary)
            dirname, basename = os.path.split(binary_path)
            link_path = os.path.join(dirname, "yt_" + basename)
            replace_symlink(binary_path, link_path)


def get_default_python_root():
    return apply_multiple(times=2, func=os.path.dirname, argument=os.path.abspath(__file__))


def get_default_yt_root():
    return apply_multiple(times=3, func=os.path.dirname, argument=os.path.abspath(__file__))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--python-root", default=get_default_python_root())
    parser.add_argument("--yt-root", default=get_default_yt_root())
    parser.add_argument("--arcadia-root", default=get_default_yt_root())
    args = parser.parse_args()

    prepare_python_source_tree(python_root=args.python_root, yt_root=args.yt_root, arcadia_root=args.arcadia_root)


if __name__ == "__main__":
    main()
