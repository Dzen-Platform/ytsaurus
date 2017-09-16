#!/usr/bin/env python
import os
import sys
# TODO(asaitgalin): Maybe replace it with PYTHONPATH=... in teamcity command?
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "teamcity-build", "python"))

from teamcity import (build_step, cleanup_step, teamcity_main,
                      teamcity_message, teamcity_interact,
                      StepFailedWithNonCriticalError)

from helpers import (mkdirp, run, run_captured, cwd, copytree,
                     kill_by_name, sudo_rmtree, ls, get_size,
                     rmtree, rm_content, clear_system_tmp,
                     format_yes_no, parse_yes_no_bool, cleanup_cgroups,
                     ChildHasNonZeroExitCode)

from pytest_helpers import get_sandbox_dirs, save_failed_test, find_and_report_core_dumps

import argparse
import glob
import os.path
import pprint
import re
import shutil
import socket
import tarfile
import tempfile
import fnmatch
import xml.etree.ElementTree as etree
import xml.parsers.expat
import urlparse

try:
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "nanny-releaselib", "src"))
    from releaselib.sandbox import client as sandbox_client
except:
    sandbox_client = None

def yt_processes_cleanup():
    kill_by_name("^ytserver")
    kill_by_name("^node")
    kill_by_name("^run_proxy")

@build_step
def prepare(options, build_context):
    os.environ["LANG"] = "en_US.UTF-8"
    os.environ["LC_ALL"] = "en_US.UTF-8"

    script_directory = os.path.dirname(os.path.realpath(__file__))

    options.build_number = os.environ["BUILD_NUMBER"]
    options.build_vcs_number = os.environ["BUILD_VCS_NUMBER"]
    options.build_git_depth = run_captured(
        [os.path.join(script_directory, "git-depth.py")],
        cwd=options.checkout_directory)

    options.build_enable_nodejs = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_NODEJS", "YES"))
    options.build_enable_python_2_6 = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PYTHON_2_6", "YES"))
    options.build_enable_python_2_7 = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PYTHON_2_7", "YES"))
    options.build_enable_python_skynet = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PYTHON_SKYNET", "YES"))
    options.build_enable_perl = parse_yes_no_bool(os.environ.get("BUILD_ENABLE_PERL", "YES"))

    options.use_asan = parse_yes_no_bool(os.environ.get("USE_ASAN", "NO"))
    options.use_tsan = parse_yes_no_bool(os.environ.get("USE_TSAN", "NO"))
    options.use_msan = parse_yes_no_bool(os.environ.get("USE_MSAN", "NO"))
    options.use_asan = options.use_asan or parse_yes_no_bool(os.environ.get("BUILD_ENABLE_ASAN", "NO"))  # compat

    options.git_branch = options.branch
    options.branch = re.sub(r"^refs/heads/", "", options.branch)
    options.branch = options.branch.split("/")[0]

    codename = run_captured(["lsb_release", "-c"])
    codename = re.sub(r"^Codename:\s*", "", codename)

    if codename not in ["lucid", "precise", "trusty"]:
        raise RuntimeError("Unknown LSB distribution code name: {0}".format(codename))

    if codename == "lucid":
        options.build_enable_python = options.build_enable_python_2_6
    elif codename in ["precise", "trusty"]:
        options.build_enable_python = options.build_enable_python_2_7

    options.codename = codename
    extra_repositories = filter(lambda x: x != "", map(str.strip, os.environ.get("EXTRA_REPOSITORIES", "").split(",")))
    options.repositories = ["yt-" + codename] + extra_repositories

    # Now determine the compiler.
    options.cc = run_captured(["which", options.cc])
    options.cxx = run_captured(["which", options.cxx])

    if not options.cc:
        raise RuntimeError("Failed to locate C compiler")

    if not options.cxx:
        raise RuntimeError("Failed to locate CXX compiler")

    # options.use_lto = (options.type != "Debug")
    options.use_lto = False

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

    clear_system_tmp()

    yt_processes_cleanup()

    # Clean core path from previous builds.
    rm_content(options.core_path)

    mkdirp(options.sandbox_directory)

    os.chdir(options.sandbox_directory)

    teamcity_message(pprint.pformat(options.__dict__))


@build_step
def configure(options, build_context):
    run([
        "cmake",
        "-DCMAKE_INSTALL_PREFIX=/usr",
        "-DCMAKE_BUILD_TYPE={0}".format(options.type),
        "-DCMAKE_COLOR_MAKEFILE:BOOL=OFF",
        "-DYT_BUILD_ENABLE_EXPERIMENTS:BOOL=ON",
        "-DYT_BUILD_ENABLE_TESTS:BOOL=ON",
        "-DYT_BUILD_ENABLE_GDB_INDEX:BOOL=ON",
        "-DYT_BUILD_BRANCH={0}".format(options.branch),
        "-DYT_BUILD_NUMBER={0}".format(options.build_number),
        "-DYT_BUILD_VCS_NUMBER={0}".format(options.build_vcs_number[0:7]),
        "-DYT_BUILD_GIT_DEPTH={0}".format(options.build_git_depth),
        "-DYT_BUILD_ENABLE_NODEJS={0}".format(format_yes_no(options.build_enable_nodejs)),
        "-DYT_BUILD_ENABLE_PYTHON_2_6={0}".format(format_yes_no(options.build_enable_python_2_6)),
        "-DYT_BUILD_ENABLE_PYTHON_2_7={0}".format(format_yes_no(options.build_enable_python_2_7)),
        "-DYT_BUILD_ENABLE_PYTHON_SKYNET={0}".format(format_yes_no(options.build_enable_python_skynet)),
        "-DYT_BUILD_ENABLE_PERL={0}".format(format_yes_no(options.build_enable_perl)),
        "-DYT_USE_ASAN={0}".format(format_yes_no(options.use_asan)),
        "-DYT_USE_TSAN={0}".format(format_yes_no(options.use_tsan)),
        "-DYT_USE_MSAN={0}".format(format_yes_no(options.use_msan)),
        "-DYT_USE_LTO={0}".format(format_yes_no(options.use_lto)),
        "-DCMAKE_CXX_COMPILER={0}".format(options.cxx),
        "-DCMAKE_C_COMPILER={0}".format(options.cc),
        "-DBUILD_SHARED_LIBS=OFF",
        options.checkout_directory],
        cwd=options.working_directory)


@build_step
def build(options, build_context):
    cpus = int(os.sysconf("SC_NPROCESSORS_ONLN"))
    run(["make", "-j", str(cpus)], cwd=options.working_directory, silent_stdout=True)

@build_step
def set_suid_bit(options, build_context):
    for binary in ["ytserver-node", "ytserver-exec", "ytserver-job-proxy", "ytserver-tools"]:
        path = "{0}/bin/{1}".format(options.working_directory, binary)
        run(["sudo", "chown", "root", path])
        run(["sudo", "chmod", "4755", path])

@build_step
def package(options, build_context):
    if not options.package:
        return

    with cwd(options.working_directory):
        run(["make", "-j", "8", "package"])
        run(["make", "-j", "8", "python-package"])
        run(["make", "version"])

        with open("ytversion") as handle:
            version = handle.read().strip()
        build_context["yt_version"] = version

        teamcity_message("We have built a package")
        teamcity_interact("setParameter", name="yt.package_built", value=1)
        teamcity_interact("setParameter", name="yt.package_version", value=version)
        teamcity_interact("buildStatus", text="{{build.status.text}}; Package: {0}".format(version))

        artifacts = glob.glob("./ARTIFACTS/yandex-yt*{0}*.changes".format(version))
        if artifacts:
            for repository in options.repositories:
                run(["dupload", "--to", repository, "--nomail", "--force"] + artifacts)
                teamcity_message("We have uploaded a package to " + repository)
                teamcity_interact("setParameter", name="yt.package_uploaded." + repository, value=1)

@build_step
def run_prepare(options, build_context):
    nodejs_source = os.path.join(options.checkout_directory, "yt", "nodejs")
    nodejs_build = os.path.join(options.working_directory, "yt", "nodejs")

    yt_node_binary_path = os.path.join(nodejs_source, "lib", "ytnode.node")
    run(["rm", "-f", yt_node_binary_path])
    run(["ln", "-s", os.path.join(nodejs_build, "ytnode.node"), yt_node_binary_path])

    with cwd(nodejs_build):
        if os.path.exists("node_modules"):
            rmtree("node_modules")
        run(["npm", "install"])

    link_path = os.path.join(nodejs_build, "node_modules", "yt")
    run(["rm", "-f", link_path])
    run(["ln", "-s", nodejs_source, link_path])

@build_step
def run_sandbox_upload(options, build_context):
    if not options.package or sys.version_info < (2, 7):
        return

    build_context["sandbox_upload_root"] = os.path.join(options.working_directory, "sandbox_upload")
    sandbox_ctx = {"upload_urls": {}}
    binary_distribution_folder = os.path.join(build_context["sandbox_upload_root"], "bin")
    mkdirp(binary_distribution_folder)

    def sky_share(resource, cwd):
        run_result = run(
            ["sky", "share", resource],
            cwd=cwd,
            shell=False,
            timeout=100,
            capture_output=True)

        rbtorrent = run_result.stdout.splitlines()[0].strip()
        # simple sanity check
        if urlparse.urlparse(rbtorrent).scheme != "rbtorrent":
            raise RuntimeError("Failed to parse rbtorrent url: {0}".format(rbtorrent))
        return rbtorrent

    # Prepare binary distribution folder
    # {working_directory}/bin contains lots of extra binaries,
    # filter daemon binaries by prefix "ytserver-"

    source_binary_root = os.path.join(options.working_directory, "bin")
    processed_files = set()
    for filename in os.listdir(source_binary_root):
        if not filename.startswith("ytserver-"):
            continue
        source_path = os.path.join(source_binary_root, filename)
        destination_path = os.path.join(binary_distribution_folder, filename)
        if not os.path.isfile(source_path):
            teamcity_message("Skip non-file item {0}".format(filename))
            continue
        teamcity_message("Symlink {0} to {1}".format(source_path, destination_path))
        os.symlink(source_path, destination_path)
        processed_files.add(filename)

    yt_binary_upload_list = set((
        "ytserver-job-proxy",
        "ytserver-scheduler",
        "ytserver-master",
        "ytserver-core-forwarder",
        "ytserver-exec",
        "ytserver-node",
        "ytserver-proxy",
        "ytserver-tools",
    ))
    if yt_binary_upload_list - processed_files:
        missing_file_string = ", ".join(yt_binary_upload_list - processed_files)
        teamcity_message("Missing files in sandbox upload: {0}".format(missing_file_string), "WARNING")

    rbtorrent = sky_share(
            os.path.basename(binary_distribution_folder),
            os.path.dirname(binary_distribution_folder))
    sandbox_ctx["upload_urls"]["yt_binaries"] = rbtorrent

    # Nodejs package
    nodejs_tar = os.path.join(build_context["sandbox_upload_root"],  "node_modules.tar")
    nodejs_build = os.path.join(options.working_directory, "debian/yandex-yt-http-proxy/usr/lib/node_modules")
    with tarfile.open(nodejs_tar, "w", dereference=True) as tar:
        tar.add(nodejs_build, arcname="/node_modules", recursive=True)

    rbtorrent = sky_share("node_modules.tar", build_context["sandbox_upload_root"])
    sandbox_ctx["upload_urls"]["node_modules"] = rbtorrent

    sandbox_ctx["git_commit"] = options.build_vcs_number
    sandbox_ctx["git_branch"] = options.git_branch
    sandbox_ctx["build_number"] = options.build_number

    #
    # Start sandbox task
    #

    version_by_teamcity = "{0}@{1}-{2}".format(options.git_branch, options.build_number, options.build_vcs_number[:7])
    cli = sandbox_client.SandboxClient(oauth_token=os.environ["TEAMCITY_SANDBOX_TOKEN"])
    task_id = cli.create_task(
        "YT_UPLOAD_RESOURCES",
        "YT_ROBOT",
        "Yt build: {0}".format(version_by_teamcity),
        sandbox_ctx)
    teamcity_message("Created sandbox upload task: {0}".format(task_id))
    teamcity_message("Check at: https://sandbox.yandex-team.ru/task/{0}/view".format(task_id))
    build_context["sandbox_upload_task"] = task_id

    teamcity_interact("setParameter", name="yt.sandbox_task_id", value=task_id)
    teamcity_interact("setParameter", name="yt.sandbox_task_url",
                      value="https://sandbox.yandex-team.ru/task/{0}/view".format(task_id))
    status = "{{build.status.text}}; Package: {0}; SB: {1}".format(build_context["yt_version"], task_id)
    teamcity_interact("buildStatus", text=status)

@build_step
def run_unit_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("Skipping unit tests since tests are disabled")
        return

    sandbox_current = os.path.join(options.sandbox_directory, "unit_tests")
    sandbox_archive = os.path.join(options.failed_tests_path,
        "__".join([options.btid, options.build_number, "unit_tests"]))

    all_unittests = fnmatch.filter(os.listdir(os.path.join(options.working_directory, "bin")), "unittester*")

    mkdirp(sandbox_current)
    try:
        for unittest_binary in all_unittests:
            run([
                "gdb",
                "--batch",
                "--return-child-result",
                "--command={0}/scripts/teamcity-build/teamcity-gdb-script".format(options.checkout_directory),
                "--args",
                os.path.join(options.working_directory, "bin", unittest_binary),
                "--gtest_color=no",
                "--gtest_death_test_style=threadsafe",
                "--gtest_output=xml:" + os.path.join(options.working_directory, "gtest_" + unittest_binary + ".xml")],
                cwd=sandbox_current,
                timeout=20 * 60)
    except ChildHasNonZeroExitCode as err:
        teamcity_message('Copying unit tests sandbox from "{0}" to "{1}"'.format(
            sandbox_current, sandbox_archive), status="WARNING")
        copytree(sandbox_current, sandbox_archive)
        for unittest_binary in all_unittests:
            shutil.copy2(
                os.path.join(options.working_directory, "bin", unittest_binary),
                os.path.join(sandbox_archive, unittest_binary))

        raise StepFailedWithNonCriticalError(str(err))
    finally:
        find_and_report_core_dumps(options, "unit_tests", sandbox_current)
        rmtree(sandbox_current)


@build_step
def run_javascript_tests(options, build_context):
    if not options.build_enable_nodejs or options.disable_tests:
        return

    tests_path = "{0}/yt/nodejs".format(options.working_directory)

    try:
        run(
            ["./run_tests.sh", "-R", "xunit"],
            cwd=tests_path,
            env={"MOCHA_OUTPUT_FILE": "{0}/junit_nodejs_run_tests.xml".format(options.working_directory)})
    except ChildHasNonZeroExitCode as err:
        raise StepFailedWithNonCriticalError(str(err))
    finally:
        find_and_report_core_dumps(options, "javascript", tests_path)


def run_pytest(options, suite_name, suite_path, pytest_args=None, env=None):
    yt_processes_cleanup()

    if not options.build_enable_python:
        return

    if pytest_args is None:
        pytest_args = []

    sandbox_current, sandbox_storage = get_sandbox_dirs(options, suite_name)
    mkdirp(sandbox_current)

    failed = False

    if env is None:
        env = {}

    env["PATH"] = "{0}/bin:{0}/yt/nodejs:/usr/sbin:{1}".format(options.working_directory, os.environ.get("PATH", ""))
    env["PYTHONPATH"] = "{0}/python:{1}".format(options.checkout_directory, os.environ.get("PYTHONPATH", ""))
    env["TESTS_SANDBOX"] = sandbox_current
    env["TESTS_SANDBOX_STORAGE"] = sandbox_storage
    env["YT_CAPTURE_STDERR_TO_FILE"] = "1"
    env["YT_ENABLE_VERBOSE_LOGGING"] = "1"
    for var in ["TEAMCITY_YT_TOKEN", "TEAMCITY_SANDBOX_TOKEN"]:
        if var in os.environ:
            env[var] = os.environ[var]

    with tempfile.NamedTemporaryFile() as handle:
        try:
            run([
                "py.test",
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
            if err.return_code == 66:
                teamcity_interact("buildProblem", description="Too many executors crashed during {0} tests. "
                                                              "Test session was terminated.".format(suite_name))

            teamcity_message("(ignoring child failure since we are reading test results from XML)")
            failed = True

        if hasattr(etree, "ParseError"):
            ParseError = etree.ParseError
        else:
            # Lucid case.
            ParseError = TypeError


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

        except (UnicodeDecodeError, ParseError, xml.parsers.expat.ExpatError):
            failed = True
            teamcity_message("Failed to parse pytest output:\n" + open(handle.name).read())

    cores_found = find_and_report_core_dumps(options, suite_name, suite_path)

    try:
        if failed or cores_found:
            save_failed_test(options, suite_name, suite_path)
            raise StepFailedWithNonCriticalError("Tests '{0}' failed".format(suite_name))
    finally:
        # Note: ytserver tests may create files with that cannot be deleted by teamcity user.
        sudo_rmtree(sandbox_current)
        if os.path.exists(sandbox_storage):
            sudo_rmtree(sandbox_storage)

@build_step
def run_integration_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("Integration tests are skipped since all tests are disabled")
        return

    pytest_args = []
    if options.enable_parallel_testing:
        pytest_args.extend(["--process-count", "6"])

    run_pytest(options, "integration", "{0}/tests/integration".format(options.checkout_directory),
               pytest_args=pytest_args)

@build_step
def run_cpp_integration_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("C++ integration tests are skipped since all tests are disabled")
        return
    run_pytest(options, "cpp_integration", "{0}/tests/cpp".format(options.checkout_directory))

@build_step
def run_yp_integration_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("YP integration tests are skipped since all tests are disabled")
        return
    
    node_path = os.path.join(options.working_directory, "yt", "nodejs", "node_modules")
    run_pytest(options, "yp_integration", "{0}/yp/tests".format(options.checkout_directory),
               env={
                   "NODE_PATH": node_path
               })

@build_step
def run_python_libraries_tests(options, build_context):
    if options.disable_tests:
        teamcity_message("Python tests are skipped since all tests are disabled")
        return

    pytest_args = []
    if options.enable_parallel_testing:
        pytest_args.extend(["--process-count", "4"])

    node_path = os.path.join(options.working_directory, "yt", "nodejs", "node_modules")
    run_pytest(options, "python_libraries", "{0}/python".format(options.checkout_directory),
               pytest_args=pytest_args,
               env={
                   "TESTS_JOB_CONTROL": "1",
                   "YT_ENABLE_REQUEST_LOGGING": "1",
                   "NODE_PATH": node_path
                })

@build_step
def run_perl_tests(options, build_context):
    if not options.build_enable_perl or options.disable_tests:
        return
    run_pytest(options, "perl", "{0}/perl/tests".format(options.checkout_directory))

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
def clean_failed_tests(options, build_context, max_allowed_size=None):
    if options.is_bare_metal:
        max_allowed_size = 500 * 1024 * 1024 * 1024
    else:
        max_allowed_size = 50 * 1024 * 1024 * 1024

    should_remove = False
    total_size = 0
    for path in ls(options.failed_tests_path,
                   select=os.path.isdir,
                   stop=sys.maxint):
        size = get_size(path, enable_cache=True)
        if total_size + size > max_allowed_size:
            should_remove = True

        if should_remove:
            teamcity_message("Removing {0}...".format(path), status="WARNING")
            if os.path.isdir(path):
                rmtree(path)
                if os.path.exists(path + ".size"):
                    os.remove(path + ".size")
            else:
                os.unlink(path)
        else:
            total_size += size


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

    options = parser.parse_args()
    options.failed_tests_path = os.path.expanduser("~/failed_tests")
    options.core_path = os.path.expanduser("~/core")
    options.is_bare_metal = socket.getfqdn().endswith("tc.yt.yandex.net")
    # NB: parallel testing is enabled by default only for bare metal machines.
    options.enable_parallel_testing = options.is_bare_metal

    teamcity_main(options)


if __name__ == "__main__":
    main()

