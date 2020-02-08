#!/usr/bin/env python
import os
import sys
# TODO(asaitgalin): Maybe replace it with PYTHONPATH=... in teamcity command?
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "teamcity-build", "python"))

from teamcity.teamcity import (
    build_step,
    cleanup_step,
    teamcity_main,
    teamcity_message,
    teamcity_interact,
    StepFailedWithNonCriticalError,
)

from teamcity.helpers import (
    ChildHasNonZeroExitCode,
    cleanup_cgroups,
    clear_system_tmp,
    cwd,
    kill_by_name,
    ls,
    mkdirp,
    parse_yes_no_bool,
    rm_content,
    rmtree,
    run,
    run_captured,
    run_parallel,
    sudo_rmtree,
    dch,
)

from teamcity.pytest_helpers import (
    archive_core_dumps_if_any,
    get_sandbox_dirs,
    save_failed_test,
    clean_failed_tests_directory,
    prepare_python_bindings,
)

from teamcity.ya import run_ya_command_with_retries

from datetime import datetime

import argparse
import contextlib
import fnmatch
import functools
import glob
import json
import os.path
import pprint
import re
import resource
import shutil
import socket
import sys
import tarfile
import tempfile
import time
import urlparse
import xml.etree.ElementTree as etree
import xml.parsers.expat


import urllib3
urllib3.disable_warnings()
import requests

KB = 1024
MB = 1024 * KB
GB = 1024 * MB
TB = 1024 * GB

PACKAGING_TIMEOUT = 30 * 60

INTEGRATION_TESTS_PARALLELISM = {"debug": 4, "release": 10, "debug-asan": 3, "release-asan": 3}
PYTHON_TESTS_PARALLELISM = 4
YP_TESTS_PARALLELISM = 6

YA_CACHE_YT_STORE_PROXY = "freud"
YA_CACHE_YT_DIR = "//home/yt-teamcity-build/cache"

YA_CACHE_YT_MAX_STORE_SIZE = 2 * TB
YA_CACHE_YT_STORE_TTL = 24  # hours
YA_CACHE_YT_STORE_CODEC = "zstd08_1"

try:
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "nanny-releaselib", "src"))
    from releaselib.sandbox import client as sandbox_client
except:
    sandbox_client = None

def yt_processes_cleanup():
    kill_by_name("^ytserver")
    kill_by_name("^node")

def comma_separated_set(s):
    return set(el for el in s.split(",") if el)

def process_core_dumps(options, suite_name, suite_path):
    sandbox_archive = os.path.join(
        options.failed_tests_path,
        "__".join([options.btid, options.build_number, suite_name]))

    return archive_core_dumps_if_any(
        core_dump_search_dir_list=[suite_path, options.core_path],
        working_directory=options.working_directory,
        archive_dir=sandbox_archive)

def python_package_path(options):
    return os.path.join(options.checkout_directory, "scripts/teamcity-build/python/python_packaging/package.py")

def only_for_projects(*projects):
    def decorator(func):
        @functools.wraps(func)
        def wrapped_function(options, build_context):
            if not any(p in options.build_project for p in projects):
                teamcity_message("Skipping step {0} due to build_project configuration".format(func.__name__))
                return
            func(options, build_context)
        return wrapped_function
    return decorator

def get_artifacts_dir(options):
    return os.path.join(options.working_directory, "ARTIFACTS")

def get_bin_dir(options):
    return os.path.join(options.working_directory, "bin")

def get_yt_token_file(options):
    return os.path.join(options.working_directory, "yt_token")

@contextlib.contextmanager
def temporary_yt_token_file(options):
    filename = get_yt_token_file(options)

    try:
        with open(filename, "w") as outf:
            outf.write(os.environ["TEAMCITY_YT_TOKEN"])
            outf.write("\n")
        yield
    finally:
        if os.path.exists(filename):
            os.remove(filename)

@contextlib.contextmanager
def inside_temporary_directory(dir=None):
    curdir = os.getcwd()
    directory = tempfile.mkdtemp(dir=dir)
    os.chdir(directory)
    try:
        yield directory
    finally:
        shutil.rmtree(directory)
        os.chdir(curdir)

def reset_debian_changelog():
    if not os.path.exists("debian"):
        os.mkdir("debian")
    if os.path.lexists("debian/changelog"):
        os.remove("debian/changelog")

def get_python_packages_config(options):
    config_generator = os.path.join(get_lib_dir_for_python(options, "2.7"), "build_python_packages_config_generator")
    return json.loads(run_captured([config_generator]))

def get_lib_dir_for_python(options, python_version):
    return os.path.join(
        options.working_directory,
        "lib",
        "pyshared-" + python_version.replace(".", "-"))

def iter_enabled_python_versions(options, enable_skynet=False):
    if options.build_enable_python_2_7:
        yield "2.7"

    if options.build_enable_python_3_4:
        yield "3.4"

    if enable_skynet:
        yield "skynet"

def iter_enabled_python_platforms(options):
    if options.ya_target_platform:
        yield options.ya_target_platform
    else:
        yield "linux"
        # Darwin build is used only for building python packages in release build,
        # but we want to check that everything is ok in debug builds too.
        yield "darwin"

def get_ya(options):
    return os.path.join(options.checkout_directory, "ya")

def get_ya_cache_dir(options):
    ya_cache = os.environ.get("YA_CACHE_DIR", None)
    if ya_cache is None:
        ya_cache = os.path.join(options.working_directory, "ya_cache")
    return ya_cache

def ya_make_env(options):
    return {
        "YA_CACHE_DIR": get_ya_cache_dir(options),
        "SVN_SSH": "ssh -v -l robot-yt-openstack ",
    }

def ya_make_definition_args(options):
    # This args cannot be passed to ya package.
    return [
        "-DYT_ENABLE_GDB_INDEX=yes",
        "-DYT_VERSION_PATCH={0}".format(options.patch_number),
        "-DYT_VERSION_BRANCH={0}".format(options.branch),
    ]

def ya_make_yt_store_args(options):
    return [
        "--yt-store",
        "--yt-put",
        "--yt-proxy", YA_CACHE_YT_STORE_PROXY,
        "--yt-dir", YA_CACHE_YT_DIR,
        "--yt-store-codec", YA_CACHE_YT_STORE_CODEC,
        "--yt-max-store-size", str(YA_CACHE_YT_MAX_STORE_SIZE),
        "--yt-store-ttl", str(YA_CACHE_YT_STORE_TTL),
        "--yt-token-path", get_yt_token_file(options),
    ]

def ya_make_args(options):
    args = ["--build", options.ya_build_type]
    if options.use_thinlto:
        args += ["--thinlto"]
    if options.ya_target_platform:
        args += ["--target-platform", options.ya_target_platform]
    return args

@build_step
def prepare(options, build_context):
    os.environ["LANG"] = "en_US.UTF-8"
    os.environ["LC_ALL"] = "en_US.UTF-8"

    options.build_number = os.environ["BUILD_NUMBER"]
    options.build_vcs_number = os.environ["BUILD_VCS_NUMBER"]

    options.build_enable_python_2_7 = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PYTHON_2_7", "YES"))
    options.build_enable_python_3_4 = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PYTHON_3_4", "YES"))
    options.build_enable_python_skynet = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PYTHON_SKYNET", "YES"))
    options.build_enable_perl = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PERL", "YES"))
    options.build_enable_ya_yt_store = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_YA_YT_STORE", "NO"))
    options.package_enable_python_yp = parse_yes_no_bool(os.environ.get("PACKAGE_ENABLE_PYTHON_YP", "NO"))
    options.package_enable_yson_bingings = parse_yes_no_bool(os.environ.get("PACKAGE_ENABLE_YSON_BINDINGS", "NO"))
    options.package_enable_rpc_bingings = parse_yes_no_bool(os.environ.get("PACKAGE_ENABLE_RPC_BINDINGS", "NO"))
    options.package_enable_driver_bingings = parse_yes_no_bool(os.environ.get("PACKAGE_ENABLE_DRIVER_BINDINGS", "NO"))
    options.build_enable_dist_build = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_DIST_BUILD", "NO"))

    options.use_asan = parse_yes_no_bool(os.environ.get("USE_ASAN", "NO"))
    assert not options.use_asan or options.build_system == "ya", "ASAN build is enabled only for --build-system=ya"

    options.use_tsan = parse_yes_no_bool(os.environ.get("USE_TSAN", "NO"))
    options.use_msan = parse_yes_no_bool(os.environ.get("USE_MSAN", "NO"))
    options.use_asan = options.use_asan or parse_yes_no_bool(os.environ.get("BUILD_ENABLE_ASAN", "NO"))  # compat

    options.git_branch = options.branch
    options.branch = re.sub(r"^refs/heads/", "", options.branch)
    options.branch = options.branch.split("/")[0]

    options.patch_number = run_captured([os.path.join(options.checkout_directory, "git-depth.py")], cwd=options.checkout_directory)

    codename = run_captured(["lsb_release", "-c"])
    codename = re.sub(r"^Codename:\s*", "", codename)

    if codename not in ["precise", "trusty"]:
        raise RuntimeError("Unknown LSB distribution code name: {0}".format(codename))

    if codename in ["precise", "trusty"]:
        options.build_enable_python = options.build_enable_python_2_7

    options.codename = codename
    extra_repositories = filter(lambda x: x != "", map(str.strip, os.environ.get("EXTRA_REPOSITORIES", "").split(",")))
    options.repositories = ["yt-" + codename] + extra_repositories
    options.bindings_repositories = ["yt-common", "common"]
    options.ya_target_platform = os.environ.get("YA_TARGET_PLATFORM", None)  # None is for default

    if options.build_system != "ya":
        # Now determine the compiler.
        options.cc = run_captured(["which", options.cc])
        options.cxx = run_captured(["which", options.cxx])

        if not options.cc:
            raise RuntimeError("Failed to locate C compiler")

        if not options.cxx:
            raise RuntimeError("Failed to locate CXX compiler")

    options.use_thinlto = False  # (options.type != "Debug")
    options.use_lto = False

    options.ya_build_type = {
        "Debug": "debug",
        "Release": "release",
        "RelWithDebInfo": "release",
    }[options.type]

    if os.path.exists(options.working_directory) and options.clean_working_directory:
        teamcity_message("Cleaning working directory...", status="WARNING")
        rmtree(options.working_directory)
    mkdirp(options.working_directory)

    if os.path.exists(options.sandbox_directory) and options.clean_sandbox_directory:
        teamcity_message("Cleaning sandbox directory...", status="WARNING")
        rmtree(options.sandbox_directory)

        sandbox_storage = os.path.expanduser("~/sandbox_storage/")
        if os.path.exists(sandbox_storage):
            rmtree(sandbox_storage)

    cleanup_cgroups()

    if options.clear_system_tmp:
        clear_system_tmp()

    yt_processes_cleanup()

    # Clean core path from previous builds.
    rm_content(options.core_path)

    # Enable cores.
    resource.setrlimit(
        resource.RLIMIT_CORE,
        (resource.RLIM_INFINITY, resource.RLIM_INFINITY))

    mkdirp(get_ya_cache_dir(options))
    mkdirp(get_bin_dir(options))
    mkdirp(options.sandbox_directory)

    os.chdir(options.sandbox_directory)

    teamcity_message(pprint.pformat(options.__dict__))

@build_step
def build(options, build_context):
    env = ya_make_env(options)
    yall = os.path.join(options.checkout_directory, "yall")
    args = [
        yall,
        "-T",
        "--yall-cmake-like-install", options.working_directory,
        "--yall-python-version-list", ",".join(iter_enabled_python_versions(options, enable_skynet=True)),
        "--yall-python-platform-list", ",".join(iter_enabled_python_platforms(options)),
    ]
    args += ya_make_args(options)
    args += ya_make_definition_args(options)

    if options.build_enable_ya_yt_store:
        env["YT_TOKEN"] = os.environ["TEAMCITY_YT_TOKEN"]
        args += [
            "--yall-enable-dist-cache",
            "--yall-dist-cache-put",
            "--yall-dist-cache-no-auto-token",
        ]
    else:
        args += [
            "--yall-disable-dist-cache"
        ]

    if options.build_enable_dist_build:
        args += [
            "--yall-enable-dist-build"
        ]

    if options.use_asan:
        args += [
            "--yall-asan-build",
        ]

    run(args, env=env, cwd=options.checkout_directory)


@build_step
def gather_build_info(options, build_context):
    build_context["yt_version"] = run_captured(
        [
            os.path.join(get_bin_dir(options), "ytserver-master"),
            "--version"
        ]).strip()
    build_context["build_time"] = datetime.now().isoformat()


@build_step
def set_suid_bit(options, build_context):
    for binary in ["ytserver-node", "ytserver-exec", "ytserver-job-proxy", "ytserver-tools"]:
        path = os.path.join(get_bin_dir(options), binary)
        if options.build_system == "ya":
            # Binaries in bindir are hardlinks to files stored in ya cache directory.
            # we don't want to change their owner and permissions since
            # it will also affect files in ya cache directory.
            # That can make ya crazy. So we replace hard links with real copy of these files.
            copy_path = path + ".copy"
            shutil.copy(path, copy_path)
            shutil.move(copy_path, path)
        run(["sudo", "chown", "root", path])
        run(["sudo", "chmod", "4755", path])

@build_step
def import_yt_wrapper(options, build_context):
    python_directory = os.path.join(options.checkout_directory, "python")

    # ya build doesn't install bindings automatically so we install it here
    # for cmake build this should do no harm
    prepare_python_bindings(python_directory, options.working_directory, "2.7")
    sys.path.insert(0, python_directory)

    try:
        import yt.wrapper
    except ImportError as err:
        raise RuntimeError("Failed to import yt wrapper: {0}".format(err))
    yt.wrapper.config["token"] = os.environ["TEAMCITY_YT_TOKEN"]
    build_context["yt.wrapper"] = yt.wrapper

def sky_get(resource):
    run(
        ["sky", "get", resource],
        timeout=300
    )

def sky_share(resource, cwd):
    run_result = run(
        ["sky", "share", resource],
        cwd=cwd,
        shell=False,
        timeout=600,
        capture_output=True)

    rbtorrent = run_result.stdout.splitlines()[0].strip()
    # simple sanity check
    if urlparse.urlparse(rbtorrent).scheme != "rbtorrent":
        raise RuntimeError("Failed to parse rbtorrent url: {0}".format(rbtorrent))
    return rbtorrent

def share_yt_deb_packages(options, build_context):
    # Share all important packages via skynet and store in sandbox.
    upload_packages = [
        "yandex-yt-src",
        "yandex-yt-http-proxy",
        "yandex-yt-proxy",
        "yandex-yt-master",
        "yandex-yt-clock",
        "yandex-yt-scheduler",
        "yandex-yt-controller-agent",
        "yandex-yt-clickhouse",
        "yandex-yt-node",
        "yandex-yt-http-proxy-dbg",
        "yandex-yt-proxy-dbg",
        "yandex-yt-master-dbg",
        "yandex-yt-clock-dbg",
        "yandex-yt-scheduler-dbg",
        "yandex-yt-controller-agent-dbg",
        "yandex-yt-clickhouse-dbg",
        "yandex-yt-node-dbg",

        "yandex-yt-python-skynet-driver",
        "yandex-yt-python-driver",
    ]

    try:
        version = build_context["yt_version"]
        build_time = build_context["build_time"]
        cli = sandbox_client.SandboxClient(oauth_token=os.environ["TEAMCITY_SANDBOX_TOKEN"])

        dir = os.path.join(options.working_directory, "./ARTIFACTS")
        rows = []
        for pkg in upload_packages:
            path = "{0}/{1}_{2}_amd64.deb".format(dir, pkg, version)
            if not os.path.exists(path):
                raise StepFailedWithNonCriticalError("Failed to find package {0} ({1})".format(pkg, path))
            else:
                torrent_id = sky_share(os.path.basename(path), os.path.dirname(path))
                sandbox_ctx = {
                    "created_resource_name": os.path.basename(path),
                    "resource_type": "YT_PACKAGE",
                    "remote_file_name": torrent_id,
                    "store_forever": True,
                    "remote_file_protocol": "skynet"}

                task_description = """
                    Build id: {0}
                    Build type: {1}
                    Source host: {2}
                    Teamcity build type id: {3}
                    Package: {4}
                    """.format(
                    version,
                    options.type,
                    socket.getfqdn(),
                    options.btid,
                    pkg)

                task_id = cli.create_task("YT_REMOTE_COPY_RESOURCE", "YT_ROBOT", task_description, sandbox_ctx)
                teamcity_message("Created sandbox upload task: package: {0}, task_id: {1}, torrent_id: {2}".format(pkg, task_id, torrent_id))
                rows.append({
                    "package": pkg,
                    "version": version,
                    "ubuntu_codename": options.codename,
                    "torrent_id": torrent_id,
                    "task_id": task_id,
                    "build_time": build_time})

        # Add to locke.
        yt_wrapper = build_context["yt.wrapper"]
        yt_wrapper.config["proxy"]["url"] = "locke"
        yt_wrapper.insert_rows("//sys/admin/skynet/packages", rows)

    except Exception as err:
        raise StepFailedWithNonCriticalError("Failed to share yt deb packages via locke and sandbox - {0}".format(err))


@build_step
def package_python_proto(options, build_context):
    if not options.package:
        teamcity_message("Skipping packaging yandex-yt-python-proto")
        return

    config = get_python_packages_config(options)

    with inside_temporary_directory(dir=options.working_directory):
        python_src_copy = os.path.realpath("python_src_copy")

        shutil.copytree(
            os.path.join(options.checkout_directory, "python"),
            python_src_copy,
            symlinks=True)

        with cwd(os.path.join(python_src_copy, "yandex-yt-python-proto")):
            reset_debian_changelog()
            dch(version=config["yt_rpc_proxy_protocol_version"],
                message="Proto package release.",
                create_package="yandex-yt-python-proto")

        run(["./build_proto.sh"], cwd=python_src_copy)

        package_list = glob.glob("yandex-yt-python-proto*")
        if not package_list:
            teamcity_message("Failed to find yandex-yt-python-proto package after build")

        artifacts_dir = get_artifacts_dir(options)
        os.mkdir(artifacts_dir)
        for file in package_list:
            shutil.move(file, os.path.join(artifacts_dir, file))

@build_step
@only_for_projects("yt")
def package(options, build_context):
    if not options.package:
        return

    with cwd(options.working_directory):
        PACKAGE_LIST = [
            "yandex-yt-controller-agent.json",
            "yandex-yt-clickhouse.json",
            "yandex-yt-http-proxy.json",
            "yandex-yt-master.json",
            "yandex-yt-clock.json",
            "yandex-yt-node.json",
            "yandex-yt-proxy.json",
            "yandex-yt-perl.json",
            "yandex-yt-perl-abi.json",
            "yandex-yt-scheduler.json",
            "yandex-yt-src.json",
        ]
        artifacts_dir = get_artifacts_dir(options)
        with cwd(artifacts_dir):
            tasks = []
            package_names = []
            for package_file in PACKAGE_LIST:
                package_file = os.path.join(get_bin_dir(options), package_file)
                with open(package_file) as inf:
                    try:
                        package_name = json.load(inf)["meta"]["name"]
                    except KeyError:
                        raise RuntimeError("Bad package file {0}, cannot find /meta/name key".format(package_file))
                    except ValueError:
                        raise RuntimeError("Bad package file {0}".format(package_file))
                args = [
                    get_ya(options), "package", package_file,
                    "--custom-version", build_context["yt_version"],
                    "--debian", "--strip", "--create-dbg",
                    "-zlow",
                ]
                args += ya_make_args(options)
                if options.use_asan:
                    args += ["--sanitize=address"]
                tasks.append(dict(
                    args=args,
                    env=ya_make_env(options),
                ))
                package_names.append(package_name)
            run_parallel(tasks, parallelism=len(tasks), timeout=PACKAGING_TIMEOUT)
            for package_name in package_names:
                expected_tar = "{}.{}.tar.gz".format(
                    package_name,
                    build_context["yt_version"])
                teamcity_message("Extracting archive {}".format(expected_tar))
                with tarfile.open(expected_tar) as tarf:
                    tarf.extractall(path=artifacts_dir)
                teamcity_message("Archive {} is extracted".format(expected_tar))

        teamcity_message("We have built a package")
        teamcity_interact("setParameter", name="yt.package_built", value=1)
        teamcity_interact("setParameter", name="yt.package_version", value=build_context["yt_version"])
        teamcity_interact("buildStatus", text="Package: {0}; {{build.status.text}}".format(build_context["yt_version"]))

        artifacts = glob.glob("./ARTIFACTS/yandex-*{0}*.changes".format(build_context["yt_version"]))
        if artifacts:
            for repository in options.repositories:
                run(["dupload", "--to", repository, "--nomail", "--force"] + artifacts)
                teamcity_message("We have uploaded a package to " + repository)
                teamcity_interact("setParameter", name="yt.package_uploaded." + repository, value=1)


@build_step
@only_for_projects("yp")
def package_yp(options, build_context):
    if not options.package:
        return

    if not options.package_enable_python_yp:
        teamcity_message("Building yandex-yp-package is disabled")
        return

    run(["./resign.sh"], cwd=os.path.join(options.checkout_directory, "yp", "python", "yandex-yp-python"))
    run(["./build_yandex_yp.sh"], cwd=os.path.join(options.checkout_directory, "yp", "python"))


def perform_python_packaging(options, packages_path, args, configurations):
    for build_type, python_type, platform, library_path, package_path, package_name in configurations:
        if python_type == "2" and not options.build_enable_python_2_7:
            continue
        if python_type == "3" and not options.build_enable_python_3_4:
            continue
        # NB: skynet enabled by default.

        run_args = args + [
            "--build-type", build_type,
            "--python-type", python_type,
            "--platform", platform,
            "--library-path", os.path.join(options.working_directory, library_path),
            "--package-path", os.path.join(packages_path, package_path),
            "--package-name", package_name,
        ]
        run(run_args, cwd=options.working_directory)


@build_step
@only_for_projects("yt")
def package_yson_bindings(options, build_context):
    if not options.package or not options.package_enable_yson_bingings:
        teamcity_message("Skipping packaging yson_bindings")
        return

    yson_packages_path = os.path.join(options.checkout_directory, "yt/python/yson-debian")
    args = [
        python_package_path(options),
        "--working-directory", options.working_directory,
        "--debian-repositories", ",".join(options.bindings_repositories),
        "--changelog-path", os.path.join(yson_packages_path, "debian/changelog"),
        "--source-python-module-path", os.path.join(options.checkout_directory, "yt/python/yt_yson_bindings"),
    ]

    configurations = [
        # build_type, python_type, library_path, package_path, package_name

        # Pypi packages.
        ("pypi", "2", "linux", "lib/pyshared-2-7/yson_lib.so", "yandex-yt-python-yson", "yandex-yt-python-yson"),
        ("pypi", "3", "linux", "lib/pyshared-3-4/yson_lib.so", "yandex-yt-python-yson", "yandex-yt-python3-yson"),
        ("pypi", "skynet", "linux", "lib/pyshared-skynet/yson_lib.so", "yandex-yt-python-yson", "yandex-yt-python-skynet-yson"),

        # Pypi packages for mac.
        ("pypi", "2", "darwin", "lib/pyshared-2-7-darwin/yson_lib.so", "yandex-yt-python-yson", "yandex-yt-python-yson"),
        ("pypi", "3", "darwin", "lib/pyshared-3-4-darwin/yson_lib.so", "yandex-yt-python-yson", "yandex-yt-python3-yson"),

        # Python-friendly debian packages.
        ("debian", "2", "linux", "lib/pyshared-2-7/yson_lib.so", "yandex-yt-python-yson", "yandex-yt-python-yson"),
        ("debian", "3", "linux", "lib/pyshared-3-4/yson_lib.so", "yandex-yt-python-yson", "yandex-yt-python3-yson"),

        # Non-python-friendly debian packages.
        ("debian", "2", "linux", "lib/pyshared-2-7/yson_lib.so", "yandex-yt-python-any-yson", "yandex-yt-python-2-7-yson"),
        ("debian", "3", "linux", "lib/pyshared-3-4/yson_lib.so", "yandex-yt-python-any-yson", "yandex-yt-python-3-4-yson"),
        ("debian", "skynet", "linux", "lib/pyshared-skynet/yson_lib.so", "yandex-yt-python-any-yson", "yandex-yt-python-skynet-yson"),
    ]

    perform_python_packaging(options, yson_packages_path, args, configurations)


@build_step
@only_for_projects("yt")
def package_rpc_bindings(options, build_context):
    if not options.package or not options.package_enable_rpc_bingings:
        teamcity_message("Skipping packaging rpc_bindings")
        return

    config = get_python_packages_config(options)

    mkdirp(os.path.join(options.working_directory, "changelogs"))
    changelog_dir = os.path.join(
        tempfile.mkdtemp(dir=os.path.join(options.working_directory, "changelogs")),
        "yandex-yt-python-driver-rpc")
    mkdirp(os.path.join(changelog_dir, "debian"))
    with cwd(changelog_dir):
        dch(version=config["yt_rpc_python_bindings_version"],
            message="Rpc driver release",
            create_package="package-name")

    rpc_packages_path = os.path.join(options.checkout_directory, "yt/python/driver-rpc-debian")
    args = [
        python_package_path(options),
        "--working-directory", options.working_directory,
        "--debian-repositories", ",".join(options.bindings_repositories),
        "--changelog-path", os.path.join(changelog_dir, "debian/changelog"),
        "--source-python-module-path", os.path.join(options.checkout_directory, "yt/python/yt_driver_rpc_bindings"),
    ]

    configurations = [
        # build_type, python_type, library_path, package_path, package_name

        # Pypi packages.
        ("pypi", "2", "linux", "lib/pyshared-2-7/driver_rpc_lib.so", "yandex-yt-python-driver-rpc", "yandex-yt-python-driver-rpc"),
        ("pypi", "3", "linux", "lib/pyshared-3-4/driver_rpc_lib.so", "yandex-yt-python-driver-rpc", "yandex-yt-python3-driver-rpc"),
        ("pypi", "skynet", "linux", "lib/pyshared-skynet/driver_rpc_lib.so", "yandex-yt-python-driver-rpc", "yandex-yt-python-skynet-driver-rpc"),

        # Pypi packages for mac.
        ("pypi", "2", "darwin", "lib/pyshared-2-7-darwin/driver_rpc_lib.so", "yandex-yt-python-driver-rpc", "yandex-yt-python-driver-rpc"),
        ("pypi", "3", "darwin", "lib/pyshared-3-4-darwin/driver_rpc_lib.so", "yandex-yt-python-driver-rpc", "yandex-yt-python3-driver-rpc"),

        # Python-friendly debian packages.
        ("debian", "2", "linux", "lib/pyshared-2-7/driver_rpc_lib.so", "yandex-yt-python-driver-rpc", "yandex-yt-python-driver-rpc"),
        ("debian", "3", "linux", "lib/pyshared-3-4/driver_rpc_lib.so", "yandex-yt-python-driver-rpc", "yandex-yt-python3-driver-rpc"),

        # Non-python-friendly debian packages.
        ("debian", "2", "linux", "lib/pyshared-2-7/driver_rpc_lib.so", "yandex-yt-python-any-driver-rpc", "yandex-yt-python-2-7-driver-rpc"),
        ("debian", "3", "linux", "lib/pyshared-3-4/driver_rpc_lib.so", "yandex-yt-python-any-driver-rpc", "yandex-yt-python-3-4-driver-rpc"),
        ("debian", "skynet", "linux", "lib/pyshared-skynet/driver_rpc_lib.so", "yandex-yt-python-any-driver-rpc", "yandex-yt-python-skynet-driver-rpc"),
    ]

    perform_python_packaging(options, rpc_packages_path, args, configurations)


@build_step
@only_for_projects("yt")
def package_driver_bindings(options, build_context):
    if not options.package or not options.package_enable_driver_bingings:
        teamcity_message("Skipping packaging driver_bindings")
        return

    config = get_python_packages_config(options)

    mkdirp(os.path.join(options.working_directory, "changelogs"))
    changelog_dir = os.path.join(
        tempfile.mkdtemp(dir=os.path.join(options.working_directory, "changelogs")),
        "yandex-yt-python-driver")
    mkdirp(os.path.join(changelog_dir, "debian"))
    with cwd(changelog_dir):
        shutil.copy(os.path.join(options.checkout_directory, "debian/changelog"), "debian/changelog")
        dch(version=config["yt_version"],
            message="Package version bump; no source changes.")
        with open("debian/changelog") as fin:
            text = fin.read()
        with open("debian/changelog", "w") as fout:
            fout.write(text.replace("yandex-yt", "package-name"))

    driver_packages_path = os.path.join(options.checkout_directory, "yt/python/driver-debian")
    args = [
        python_package_path(options),
        "--working-directory", options.working_directory,
        "--debian-repositories", "yt-common",
        "--changelog-path", os.path.join(changelog_dir, "debian/changelog"),
        "--source-python-module-path", os.path.join(options.checkout_directory, "yt/python/yt_driver_bindings"),
        "--destination", os.path.join(options.working_directory, "./ARTIFACTS"),
    ]

    configurations = [
        # build_type, python_type, library_path, package_path, package_name

        # Python-friendly debian packages.
        ("debian", "2", "linux", "lib/pyshared-2-7/driver_lib.so", "yandex-yt-python-driver", "yandex-yt-python-driver"),
        ("debian", "3", "linux", "lib/pyshared-3-4/driver_lib.so", "yandex-yt-python-driver", "yandex-yt-python3-driver"),

        # Non-python-friendly debian packages.
        ("debian", "2", "linux", "lib/pyshared-2-7/driver_lib.so", "yandex-yt-python-any-driver", "yandex-yt-python-2-7-driver-rpc"),
        ("debian", "3", "linux", "lib/pyshared-3-4/driver_lib.so", "yandex-yt-python-any-driver", "yandex-yt-python-3-4-driver-rpc"),
        ("debian", "skynet", "linux", "lib/pyshared-skynet/driver_lib.so", "yandex-yt-python-any-driver", "yandex-yt-python-skynet-driver"),
    ]

    perform_python_packaging(options, driver_packages_path, args, configurations)


@build_step
def run_sandbox_upload(options, build_context):
    if not options.package or sys.version_info < (2, 7):
        return

    # Share individual deb packages first (it is used in chef deployment).
    if options.target_build_project == "yt":
        share_yt_deb_packages(options, build_context)

    build_context["sandbox_upload_root"] = os.path.join(options.working_directory, "sandbox_upload")
    binary_distribution_folder = os.path.join(build_context["sandbox_upload_root"], "bin")
    mkdirp(binary_distribution_folder)

    # Prepare binary distribution folder
    # {working_directory}/bin contains lots of extra binaries,
    # filter daemon binaries by prefix "ytserver-"

    source_binary_root = get_bin_dir(options)
    processed_files = set()
    filename_prefix_whitelist = ["ytserver-", "ypserver-", "yt_local"]
    for filename in os.listdir(source_binary_root):
        if not any(filename.startswith(x) for x in filename_prefix_whitelist):
            continue
        source_path = os.path.join(source_binary_root, filename)
        destination_path = os.path.join(binary_distribution_folder, filename)
        if not os.path.isfile(source_path):
            teamcity_message("Skip non-file item {0}".format(filename))
            continue
        teamcity_message("Symlink {0} to {1}".format(source_path, destination_path))
        os.symlink(source_path, destination_path)
        processed_files.add(filename)

    if options.target_build_project == "yt":
        yt_binary_upload_list = set((
            "ytserver-job-proxy",
            "ytserver-scheduler",
            "ytserver-controller-agent",
            "ytserver-clickhouse",
            "ytserver-master",
            "ytserver-clock",
            "ytserver-core-forwarder",
            "ytserver-exec",
            "ytserver-node",
            "ytserver-proxy",
            "ytserver-tools",
            "ytserver-http-proxy",
            "yt_local",
        ))
    else:
        yt_binary_upload_list = set((
            "ypserver-master",
        ))

    # Check that all binaries presented.
    if yt_binary_upload_list - processed_files:
        missing_file_string = ", ".join(yt_binary_upload_list - processed_files)
        raise StepFailedWithNonCriticalError("Missing files in sandbox upload: {0}".format(missing_file_string))

    if options.target_build_project == "yt":
        # Also, inject python libraries and bindings as debs
        artifacts_directory = os.path.join(options.working_directory, "./ARTIFACTS")
        inject_packages = [
            "yandex-yt-python-skynet-driver",
            "yandex-yt-python-driver",
        ]
        for pkg in inject_packages:
            paths = glob.glob("{0}/{1}_*.deb".format(artifacts_directory, pkg))
            if len(paths) != 1:
                raise StepFailedWithNonCriticalError("Failed to find package {0}, found files {1}".format(pkg, paths))
            destination_path = os.path.join(binary_distribution_folder, os.path.basename(paths[0]))
            os.symlink(paths[0], destination_path)

    try:
        rbtorrent = sky_share(
            os.path.basename(binary_distribution_folder),
            os.path.dirname(binary_distribution_folder))

        sandbox_ctx = {}
        sandbox_ctx["upload_urls"] = {"yt_binaries": rbtorrent}
        sandbox_ctx["git_commit"] = options.build_vcs_number
        sandbox_ctx["git_branch"] = options.git_branch
        sandbox_ctx["build_number"] = options.build_number
        sandbox_ctx["full_build_type"] = options.btid
        sandbox_ctx["build_project"] = options.target_build_project

        #
        # Start sandbox task
        #

        cli = sandbox_client.SandboxClient(oauth_token=os.environ["TEAMCITY_SANDBOX_TOKEN"])
        task_description = """
        [{0}]
        YT version: {1}
        Teamcity build id: {2}
        Teamcity build type: {3}
        Teamcity host: {4}
        Teamcity build type id: {5}
        Git branch: {6}
        Git commit: {7}
        """.format(
            options.target_build_project,
            build_context["yt_version"],
            options.build_number,
            options.type,
            socket.getfqdn(),
            options.btid,
            options.git_branch,
            options.build_vcs_number,
        )

        task_id = cli.create_task(
            "YT_UPLOAD_RESOURCES",
            "YT_ROBOT",
            task_description,
            sandbox_ctx)
        teamcity_message("Created sandbox upload task: {0}".format(task_id))
        teamcity_message("Check at: https://sandbox.yandex-team.ru/task/{0}/view".format(task_id))
        build_context["sandbox_upload_task"] = task_id

        teamcity_interact("setParameter", name="yt.sandbox_task_id", value=task_id)
        teamcity_interact("setParameter", name="yt.sandbox_task_url",
                          value="https://sandbox.yandex-team.ru/task/{0}/view".format(task_id))
        status = "Package: {0}; SB: {1}; {{build.status.text}}".format(build_context["yt_version"], task_id)
        teamcity_interact("buildStatus", text=status)
    except Exception as err:
        raise StepFailedWithNonCriticalError("Failed to create YT_UPLOAD_RESOURCES task in sandbox - {0}".format(err))

@build_step
def run_unit_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("Skipping unit tests since tests are disabled")
        return

    sandbox_current = os.path.join(options.sandbox_directory, "unit_tests")

    all_unittests = fnmatch.filter(os.listdir(get_bin_dir(options)), "unittester*")

    mkdirp(sandbox_current)
    try:
        for unittest_binary in all_unittests:
            args = [
                os.path.join(get_bin_dir(options), unittest_binary),
                "--gtest_color=no",
                "--gtest_death_test_style=threadsafe",
                "--gtest_output=xml:" + os.path.join(options.working_directory, "gtest_" + unittest_binary + ".xml"),
            ]
            if not options.use_asan:
                args = [
                    "gdb",
                    "--batch",
                    "--return-child-result",
                    "--command={0}/scripts/teamcity-build/teamcity-gdb-script".format(options.checkout_directory),
                    "--args",
                ] + args
            run(args, cwd=sandbox_current, timeout=20 * 60)
    except ChildHasNonZeroExitCode as err:
        raise StepFailedWithNonCriticalError(str(err))
    finally:
        process_core_dumps(options, "unit_tests", sandbox_current)
        rmtree(sandbox_current)

def run_ya_tests(options, suite_name, test_paths, dist=True):
    # NB: tests are disabled under ASAN because random hangups in python, see YT-11297.
    if options.disable_tests or options.use_asan:
        teamcity_message("Skipping ya make tests since tests are disabled")
        return

    sandbox_current, sandbox_storage = get_sandbox_dirs(options, suite_name)
    junit_output = os.path.join(options.working_directory, "junit_yatest_{}.xml".format(suite_name))

    env = ya_make_env(options)
    if not dist:
        env["PATH"] = "{0}:/usr/sbin:{1}".format(get_bin_dir(options), os.environ.get("PATH", ""))
        env["TESTS_SANDBOX"] = sandbox_current
        env["TESTS_SANDBOX_STORAGE"] = sandbox_storage
        env["YT_CORE_PATH"] = options.core_path

    args = [
        os.path.join(options.checkout_directory, "ya"),
        "make",
        "--output", sandbox_storage,
        "--junit", junit_output,
        "--test-param", "inside_arcadia=0",
        "-ttt",
        "--dont-merge-split-tests",
        "--stat",
    ]
    if dist:
        args += ["--dist", "-E"]
    else:
        build_type = options.ya_build_type + ("-asan" if options.use_asan else "")
        args += [
            "--test-threads=" + str(INTEGRATION_TESTS_PARALLELISM[build_type]),
            "-T",
            "--test-param", "teamcity=1",
        ]

    args += test_paths
    args += ya_make_args(options)
    args += ya_make_definition_args(options)
    if options.use_asan:
        if dist:
            return # disable ya tests in ASAN build, while ya tests are flaky
        args += ["--sanitize=address"]

    def except_action():
        save_failed_test(options, suite_name, save_artifacts=False)
        if not dist:
            process_core_dumps(options, suite_name, sandbox_current)

    try:
        run_ya_command_with_retries(
            args,
            env=env,
            cwd=options.checkout_directory,
            # In case of yatest artifacts are rebuilded in dist build and included to the sandbox.
            except_action=except_action)
    finally:
        if os.path.exists(sandbox_storage):
            sudo_rmtree(sandbox_storage)

@build_step
@only_for_projects("yt")
def run_ya_integration_tests(options, build_context):
    run_ya_tests(options, "ya_integration", ["yt/tests"])

@build_step
@only_for_projects("yt")
def run_ya_integration_locally(options, build_context):
    run_ya_tests(options, "ya_integration_local", ["yt/tests"], dist=False)

def run_pytest(options, suite_name, suite_path, pytest_args=None, env=None, python_version=None):
    yt_processes_cleanup()

    if not options.build_enable_python:
        return

    if python_version is None:
        if not options.build_enable_python_2_7:
            teamcity_message("Skip test suite '{0}' since python2.7 build is disabled".format(suite_name))
            return
        else:
            python_version = "2.7"

    prepare_python_bindings(
        os.path.join(options.checkout_directory, "python"),
        options.working_directory,
        python_version)

    if pytest_args is None:
        pytest_args = []

    sandbox_current, sandbox_storage = get_sandbox_dirs(options, suite_name)
    mkdirp(sandbox_current)

    failed = False

    if env is None:
        env = {}

    env["PATH"] = "{0}:/usr/sbin:{1}".format(get_bin_dir(options), os.environ.get("PATH", ""))
    env["PYTHONPATH"] = "{0}/python:{0}/yp/python:{1}".format(
        options.checkout_directory, os.environ.get("PYTHONPATH", ""))
    env["TESTS_SANDBOX"] = sandbox_current
    env["TESTS_SANDBOX_STORAGE"] = sandbox_storage
    env["PYTEST_LOG_FILENAME"] = os.path.join(sandbox_current, "pytest_runner.log")
    env["YT_CAPTURE_STDERR_TO_FILE"] = "1"
    env["YT_ENABLE_VERBOSE_LOGGING"] = "1"
    env["YT_CORE_PATH"] = options.core_path
    if options.build_system == "ya":
        env["PERL5LIB"] = get_bin_dir(options)
    for var in ["TEAMCITY_YT_TOKEN", "TEAMCITY_SANDBOX_TOKEN"]:
        if var in os.environ:
            env[var] = os.environ[var]

    with tempfile.NamedTemporaryFile() as handle:
        try:
            run([
                "python" + python_version,
                "-m",
                "pytest",
                "-r", "x",
                "--verbose",
                "--verbose",
                "--capture=fd",
                "--tb=native",
                "--timeout=3000",
                "--debug",
                "--junitxml={0}".format(handle.name)]
                + pytest_args,
                cwd=suite_path,
                env=env)
        except ChildHasNonZeroExitCode as err:
            teamcity_interact("buildProblem", description="Pytest '{}' failed, exit code {}".format(
                suite_name, err.return_code))
            teamcity_message("(ignoring child failure since we are reading test results from XML)")
            failed = True

        try:
            result = etree.parse(handle)
            for node in (result.iter() if hasattr(result, "iter") else result.getiterator()):
                if isinstance(node.text, str):
                    node.text = node.text \
                        .replace("&quot;", "\"") \
                        .replace("&apos;", "\'") \
                        .replace("&amp;", "&") \
                        .replace("&lt;", "<") \
                        .replace("&gt;", ">")

            with open("{0}/junit_python_{1}.xml".format(options.working_directory, suite_name), "w+b") as handle:
                result.write(handle, encoding="utf-8")

        except (UnicodeDecodeError, etree.ParseError, xml.parsers.expat.ExpatError):
            failed = True
            teamcity_message("Failed to parse pytest output:\n" + open(handle.name).read())

    cores_found = process_core_dumps(options, suite_name, suite_path)

    try:
        if failed or cores_found:
            save_failed_test(options, suite_name)
            raise StepFailedWithNonCriticalError("Tests '{0}' failed".format(suite_name))
    finally:
        # Note: ytserver tests may create files with that cannot be deleted by teamcity user.
        sudo_rmtree(sandbox_current)
        if os.path.exists(sandbox_storage):
            sudo_rmtree(sandbox_storage)

@build_step
@only_for_projects("yt")
def run_yt_integration_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("Integration tests are skipped since all tests are disabled")
        return

    # Run small portion of tests using pytest. Make sure it is still working.
    pytest_args = ["-k", "TestSchedulerCommon"] 
    if options.enable_parallel_testing:
        build_type = options.ya_build_type + ("-asan" if options.use_asan else "")
        pytest_args.extend(["--process-count", str(INTEGRATION_TESTS_PARALLELISM[build_type])])

    run_pytest(options, "integration", "{0}/yt/tests/integration".format(options.checkout_directory),
               pytest_args=pytest_args)

@build_step
@only_for_projects("yt")
def run_yt_cpp_integration_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("C++ integration tests are skipped since all tests are disabled")
        return
    run_pytest(options, "cpp_integration", "{0}/yt/tests/cpp".format(options.checkout_directory))

@build_step
def run_ya_yp_tests(options, build_context):
    targets = [
        "yp/tests/py2",
        "yp/tests/py3",
    ]
    run_ya_tests(options, "ya_yp", targets)

@build_step
@only_for_projects("yt")
def run_ya_python_tests(options, build_context):
    targets = [
        "python/yt/local/tests",
        "python/yt/skiff/tests",
        "python/yt/yson/tests/py2",
        "python/yt/yson/tests/py3",
        "python/yt/wrapper/tests/py2",
        "python/yt/wrapper/tests/py3",
    ]
    run_ya_tests(options, "ya_python", targets)

@build_step
@only_for_projects("yt")
def run_python_libraries_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("Python tests are skipped since all tests are disabled")
        return
    if options.use_asan:
        teamcity_message("Python tests are skipped since they don't play well with ASAN")
        return

    pytest_args = []
    if options.enable_parallel_testing:
        pytest_args.extend(["--process-count", str(PYTHON_TESTS_PARALLELISM)])

    test_paths_filename = os.path.join(options.checkout_directory, "python/yt/wrapper/system_python_tests/test_paths.txt")
    test_names = open(test_paths_filename).read().split()
    pytest_args += [os.path.join(options.checkout_directory, "python/yt/wrapper/tests", name)
                    for name in test_names]
    run_pytest(options,
               "python_libraries",
               "{0}/python".format(options.checkout_directory),
               pytest_args=pytest_args,
               env={
                   "TESTS_JOB_CONTROL": "1",
                   "YT_ENABLE_REQUEST_LOGGING": "1",
               })

@build_step
@only_for_projects("yt")
def run_perl_tests(options, build_context):
    if not options.build_enable_perl:
        teamcity_message("Perl tests are skipped since they are not enabled")
        return
    if options.disable_tests:
        teamcity_message("Perl tests are skipped since all tests are disabled")
        return
    if options.use_asan:
        teamcity_message("Perl tests are skipped since they don't play well with ASAN")
        return
    run_pytest(options, "perl", "{0}/perl/tests".format(options.checkout_directory))


def log_sandbox_upload(options, build_context, task_id):
    client = requests.Session()
    client.headers.update({
        "Authorization": "OAuth {0}".format(os.environ["TEAMCITY_SANDBOX_TOKEN"]),
        "Accept": "application/json; charset=utf-8",
        "Content-type": "application/json",
    })
    resp = client.get("https://sandbox.yandex-team.ru/api/v1.0/task/{0}/resources".format(task_id))
    api_data = resp.json()

    resources = {}
    resource_rows = []
    for resource_item in api_data["items"]:
        if resource_item["type"] == "TASK_LOGS":
            continue
        resources.update({
            resource_item["type"]: resource_item["id"],
        })
        resource_rows.append({
            "id": resource_item["id"],
            "task_id": task_id,
            "type": resource_item["type"],
            "build_number": int(options.build_number),
            "version": build_context["yt_version"],
        })

    build_log_record = {
        "version": build_context["yt_version"],
        "build_number": int(options.build_number),
        "task_id": task_id,
        "git_branch": options.git_branch,
        "git_commit": options.build_vcs_number,
        "build_time": build_context["build_time"],
        "build_type": options.type,
        "build_host": socket.getfqdn(),
        "build_btid": options.btid,
        "ubuntu_codename": options.codename,
        "resources": resources,
    }

    # Publish YT_BINARIES resource id to build status.
    status = "Package: {0}; YT_BINARIES: {1}; {{build.status.text}}".format(build_context["yt_version"], resources["YT_BINARIES"])
    teamcity_interact("buildStatus", text=status)

    # Add to locke.
    yt_wrapper = build_context["yt.wrapper"]
    yt_wrapper.config["proxy"]["url"] = "locke"
    yt_wrapper.insert_rows("//sys/admin/skynet/builds", [build_log_record])
    yt_wrapper.insert_rows("//sys/admin/skynet/resources", resource_rows)

@build_step
def wait_for_sandbox_upload(options, build_context):
    if not options.package or sys.version_info < (2, 7):
        return

    task_id = build_context["sandbox_upload_task"]
    teamcity_message("Loaded task id: {0}".format(task_id))
    teamcity_message("Check at: https://sandbox.yandex-team.ru/task/{0}/view".format(task_id))
    cli = sandbox_client.SandboxClient(oauth_token=os.environ["TEAMCITY_SANDBOX_TOKEN"])
    try:
        cli.wait_for_complete(task_id)
    except sandbox_client.SandboxTaskError as err:
        teamcity_message("Failed waiting for task: {0}".format(err), status="WARNING")
    try:
        log_sandbox_upload(options, build_context, task_id)
    except Exception as err:
        teamcity_message("Failed to log sandbox upload: {0}".format(err), status="WARNING")

@cleanup_step
def clean_sandbox_upload(options, build_context):
    if "sandbox_upload_root" in build_context and os.path.exists(build_context["sandbox_upload_root"]):
        shutil.rmtree(build_context["sandbox_upload_root"])

@cleanup_step
def clean_artifacts(options, build_context, n=10):
    for path in ls("{0}/ARTIFACTS".format(options.working_directory),
                   reverse=True,
                   select=os.path.isfile,
                   start=n,
                   stop=sys.maxint):
        teamcity_message("Removing {0}...".format(path), status="WARNING")
        if os.path.isdir(path):
            rmtree(path)
        else:
            os.unlink(path)

@cleanup_step
def clean_binaries(options, build_context):
    for path in ls("{0}/bin".format(options.working_directory),
                   select=lambda f: os.path.isfile(f) and os.path.basename(f).startswith("ytserver-")):
        teamcity_message("Removing {0}...".format(path), status="WARNING")
        os.unlink(path)

@cleanup_step
def clean_ya_cache(options, build_context):
    ya_cache_dir = get_ya_cache_dir(options)
    for name in os.listdir(ya_cache_dir):
        if name == "logs":
            continue
        path = os.path.join(ya_cache_dir, name)
        if os.path.isdir(path):
            shutil.rmtree(path)
        else:
            os.unlink(path)


@cleanup_step
def clean_failed_tests(options, build_context, max_allowed_size=None):
    clean_failed_tests_directory(options.failed_tests_path)


################################################################################
# This is an entry-point. Just boiler-plate.

def main():
    def parse_bool(s):
        if s == "YES":
            return True
        if s == "NO":
            return False
        raise argparse.ArgumentTypeError("Expected YES or NO")
    parser = argparse.ArgumentParser(description="YT Build Script")

    parser.add_argument("--btid", type=str, action="store", required=True)
    parser.add_argument("--branch", type=str, action="store", required=True)

    parser.add_argument(
        "--checkout_directory", metavar="DIR",
        type=str, action="store", required=True)

    parser.add_argument(
        "--working_directory", metavar="DIR",
        type=str, action="store", required=True)
    parser.add_argument(
        "--clean_working_directory",
        type=parse_bool, action="store", default=False)

    parser.add_argument(
        "--sandbox_directory", metavar="DIR",
        type=str, action="store", required=True)
    parser.add_argument(
        "--clean_sandbox_directory",
        type=parse_bool, action="store", default=True)
    parser.add_argument(
        "--build_system",
        choices=["ya"], default="ya")

    parser.add_argument(
        "--clear-system-tmp",
        type=parse_bool, action="store", default=True)

    parser.add_argument(
        "--type",
        type=str, action="store", required=True, choices=("Debug", "Release", "RelWithDebInfo"))

    parser.add_argument(
        "--package",
        type=parse_bool, action="store", default=False)

    parser.add_argument(
        "--disable_tests",
        type=parse_bool, action="store", default=False)

    parser.add_argument(
        "--cc",
        type=str, action="store", required=False, default="gcc-4.8")
    parser.add_argument(
        "--cxx",
        type=str, action="store", required=False, default="g++-4.8")

    parser.add_argument(
        "--build_project",
        type=comma_separated_set, action="store", default={"yt", "yp"})

    options = parser.parse_args()
    options.failed_tests_path = os.path.expanduser("~/failed_tests")
    options.core_path = os.path.expanduser("~/core")
    options.is_bare_metal = socket.getfqdn().endswith("tc.yt.yandex.net")
    # NB: parallel testing is enabled by default only for bare metal machines.
    options.enable_parallel_testing = options.is_bare_metal
    # Determine main project of current build.
    options.target_build_project = "yt" if "yt" in options.build_project else "yp"

    teamcity_main(options)

if __name__ == "__main__":
    main()
