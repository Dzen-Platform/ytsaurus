#!/usr/bin/env python

import argparse
import contextlib
import errno
import fcntl
import glob
import itertools
import os
import os.path
import pprint
import re
import resource
import select
import socket
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import traceback
import xml.etree.ElementTree as etree

################################################################################
# These methods are used to mark actual steps to be executed.
# See the rest of the file for an example of usage.

_build_steps = []
_cleanup_steps = []


def yt_register_build_step(func):
    """Registers a build step to perform."""
    _build_steps.append(func)


def yt_register_cleanup_step(func):
    """Registers a clean-up steps to perform."""
    _cleanup_steps.append(func)


def rmtree_onerror(fn, path, excinfo):
    teamcity_message(
        "Error occured while executing {0} on '{1}': {2}".format(fn, path, excinfo),
        status="WARNING")


################################################################################
# Here are actual steps. All the meaty guts are way below.

@yt_register_build_step
def prepare(options):
    os.environ["LANG"] = "en_US.UTF-8"
    os.environ["LC_ALL"] = "en_US.UTF-8"

    options.build_number = os.environ["BUILD_NUMBER"]
    options.build_vcs_number = os.environ["BUILD_VCS_NUMBER"]

    def checked_yes_no(s):
        s = s.upper()
        if s != "YES" and s != "NO":
            raise RuntimeError("'{0}' must be either 'YES' or 'NO'".format(s))
        return s

    options.build_enable_nodejs = checked_yes_no(os.environ.get("BUILD_ENABLE_NODEJS", "YES"))
    options.build_enable_python = checked_yes_no(os.environ.get("BUILD_ENABLE_PYTHON", "YES"))
    options.build_enable_perl = checked_yes_no(os.environ.get("BUILD_ENABLE_PERL", "YES"))
    options.build_enable_llvm = checked_yes_no(os.environ.get("BUILD_ENABLE_LLVM", "YES"))

    options.branch = re.sub(r"^refs/heads/", "", options.branch)
    options.branch = options.branch.split("/")[0]

    codename = run_captured(["lsb_release", "-c"])
    codename = re.sub(r"^Codename:\s*", "", codename)

    if codename not in ["lucid", "precise", "trusty"]:
        raise RuntimeError("Unknown LSB distribution code name: {0}".format(codename))

    options.codename = codename
    options.repositories = ["yt-" + codename]

    # Now determine the compiler.
    options.cc = run_captured(["which", options.cc])
    options.cxx = run_captured(["which", options.cxx])

    if not options.cc:
        raise RuntimeError("Failed to locate C compiler")

    if not options.cxx:
        raise RuntimeError("Failed to locate CXX compiler")

    # Temporaly turn off
    # options.use_lto = (options.type != "Debug")
    options.use_lto = False

    if os.path.exists(options.working_directory) and options.clean_working_directory:
        teamcity_message("Cleaning working directory...", status="WARNING")
        shutil.rmtree(options.working_directory, onerror=rmtree_onerror)
    mkdirp(options.working_directory)

    if os.path.exists(options.sandbox_directory) and options.clean_sandbox_directory:
        teamcity_message("Cleaning sandbox directory...", status="WARNING")
        shutil.rmtree(options.sandbox_directory, onerror=rmtree_onerror)

        sandbox_storage = os.path.expanduser("~/sandbox_storage/")
        if os.path.exists(sandbox_storage):
            shutil.rmtree(sandbox_storage, onerror=rmtree_onerror)

    mkdirp(options.sandbox_directory)

    os.chdir(options.sandbox_directory)

    teamcity_message(pprint.pformat(options.__dict__))


@yt_register_build_step
def configure(options):
    run([
        "cmake",
        "-DCMAKE_INSTALL_PREFIX=/usr",
        "-DCMAKE_BUILD_TYPE={0}".format(options.type),
        "-DCMAKE_COLOR_MAKEFILE:BOOL=OFF",
        "-DYT_BUILD_ENABLE_EXPERIMENTS:BOOL=ON",
        "-DYT_BUILD_ENABLE_TESTS:BOOL=ON",
        "-DYT_BUILD_BRANCH={0}".format(options.branch),
        "-DYT_BUILD_NUMBER={0}".format(options.build_number),
        "-DYT_BUILD_VCS_NUMBER={0}".format(options.build_vcs_number[0:7]),
        "-DYT_BUILD_ENABLE_NODEJS={0}".format(options.build_enable_nodejs),
        "-DYT_BUILD_ENABLE_PYTHON={0}".format(options.build_enable_python),
        "-DYT_BUILD_ENABLE_PERL={0}".format(options.build_enable_perl),
        "-DYT_BUILD_ENABLE_LLVM={0}".format(options.build_enable_llvm),
        "-DYT_USE_LTO={0}".format(options.use_lto),
        "-DCMAKE_CXX_COMPILER={0}".format(options.cxx),
        "-DCMAKE_C_COMPILER={0}".format(options.cc),
        options.checkout_directory],
        cwd=options.working_directory)


@yt_register_build_step
def fast_build(options):
    cpus = int(os.sysconf("SC_NPROCESSORS_ONLN"))
    try:
        run(["make", "-j", str(cpus)], cwd=options.working_directory, silent_stdout=True)
    except ChildHasNonZeroExitCode:
        teamcity_message("(ignoring child failure to provide meaningful diagnostics in `slow_build`)")


@yt_register_build_step
def slow_build(options):
    run(["make"], cwd=options.working_directory)


@yt_register_build_step
def set_suid_bit(options):
    path = "{0}/bin/ytserver".format(options.working_directory)
    run(["sudo", "chown", "root", path])
    run(["sudo", "chmod", "4755", path])


@yt_register_build_step
def package(options):
    if not options.package:
        return

    with cwd(options.working_directory):
        run(["make", "-j", "8", "package"])
        run(["make", "-j", "8", "python-package"])
        run(["make", "version"])

        with open("ytversion") as handle:
            version = handle.read().strip()

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


@yt_register_build_step
def run_prepare(options):
    with cwd(options.checkout_directory):
        run(["make", "-C", "./python/yt/wrapper"])
        run(["make", "-C", "./python", "version"])

    with cwd(options.working_directory, "yt/nodejs"):
        if os.path.exists("node_modules"):
            shutil.rmtree("node_modules")
        run(["npm", "install"])


@yt_register_build_step
def run_unit_tests(options):
    try:
        run([
            "gdb",
            "--batch",
            "--return-child-result",
            "--command={0}/scripts/teamcity-gdb-script".format(options.checkout_directory),
            "--args",
            "./bin/unittester",
            "--gtest_color=no",
            "--gtest_death_test_style=threadsafe",
            "--gtest_output=xml:gtest_unittester.xml"],
            cwd=options.working_directory)
        # Removing core dumps, generated by unittester
        run("find . -maxdepth 1 -mtime +3 -exec rm {} \;",
            cwd=options.working_directory,
            shell=True)
    except ChildHasNonZeroExitCode as err:
        raise StepFailedWithNonCriticalError(str(err))


@yt_register_build_step
def run_javascript_tests(options):
    if options.build_enable_nodejs != "YES":
        return

    try:
        run(
            ["./run_tests.sh", "-R", "xunit"],
            cwd="{0}/yt/nodejs".format(options.working_directory),
            env={"MOCHA_OUTPUT_FILE": "{0}/junit_nodejs_run_tests.xml".format(options.working_directory)})
    except ChildHasNonZeroExitCode as err:
        raise StepFailedWithNonCriticalError(str(err))


def run_pytest(options, suite_name, suite_path, pytest_args=None):
    if options.build_enable_python != "YES":
        return

    if pytest_args is None:
        pytest_args = []

    sandbox_current = "{0}/{1}".format(options.sandbox_directory, suite_name)
    sandbox_archive = "{0}/{1}".format(
        os.path.expanduser("~/failed_tests/"),
        "__".join([options.btid, options.build_number, suite_name]))
    sandbox_storage = "{0}/{1}".format(os.path.expanduser("~/sandbox_storage/"), suite_name)
    working_files_to_archive = ["bin/yt", "bin/ytserver", "lib/*.so"]

    mkdirp(sandbox_current)

    failed = False

    with tempfile.NamedTemporaryFile() as handle:
        try:
            run([
                "py.test",
                "-r", "x",
                "--verbose",
                "--capture=fd",
                "--tb=native",
                "--timeout=300",
                "--debug",
                "--junitxml={0}".format(handle.name)]
                + pytest_args,
                cwd=suite_path,
                env={
                    "PATH": "{0}/bin:{0}/yt/nodejs:{1}".format(options.working_directory, os.environ.get("PATH", "")),
                    "PYTHONPATH": "{0}/python:{1}".format(options.checkout_directory, os.environ.get("PYTHONPATH", "")),
                    "TESTS_SANDBOX": sandbox_current,
                    "TESTS_SANDBOX_STORAGE": sandbox_storage
                })
        except ChildHasNonZeroExitCode:
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

        except (UnicodeDecodeError, ParseError):
            failed = True
            teamcity_message("Failed to parse pytest output:\n" + open(handle.name).read())

    try:
        if failed:
            for dir in [sandbox_storage, sandbox_current]:
                if not os.path.exists(dir):
                    continue
                teamcity_message("Copying failed tests from '{0}' to {1}'...".format(
                    dir,
                    sandbox_archive),
                    status="WARNING")
                for obj in os.listdir(dir):
                    src = os.path.join(dir, obj)
                    dst = os.path.join(sandbox_archive, obj)
                    if os.path.exists(dst):
                        shutil.rmtree(dst)
                    shutil.copytree(src, dst)
            artifact_path = os.path.join(sandbox_archive, "artifacts")
            core_dumps_path = os.path.join(sandbox_archive, "core_dumps")
            mkdirp(artifact_path)
            mkdirp(core_dumps_path)
            for fileglob in working_files_to_archive:
                for file in glob.glob(os.path.join(options.working_directory, fileglob)):
                    shutil.copy(file, artifact_path)
            for dir, _, files in os.walk(suite_path):
                for file in files:
                    if file.startswith("core."):
                        shutil.copy(os.path.join(dir, file), core_dumps_path)
            raise StepFailedWithNonCriticalError("Tests '{0}' failed".format(suite_name))
    finally:
        shutil.rmtree(sandbox_current)


def kill_by_name(name):
    # Cannot use check_output because of python2.6 on Lucid
    pids = run_captured(["pgrep", "-f", name, "-u", "teamcity"], ignore_return_code=True)
    for pid in pids.split("\n"):
        if not pid:
            continue
        os.kill(int(pid), signal.SIGKILL)


@yt_register_build_step
def run_integration_tests(options):
    kill_by_name("^ytserver")

    pytest_args = []
    if options.enable_parallel_testing:
        pytest_args.extend(["--process-count", "6"])

    run_pytest(options, "integration", "{0}/tests/integration".format(options.checkout_directory),
               pytest_args=pytest_args)


@yt_register_build_step
def run_python_libraries_tests(options):
    kill_by_name("^ytserver")
    kill_by_name("^node")

    pytest_args = []
    if options.enable_parallel_testing:
        pytest_args.extend(["--process-count", "4"])

    run_pytest(options, "python_libraries", "{0}/python".format(options.checkout_directory),
               pytest_args=["--ignore=pyinstaller"] + pytest_args)


@yt_register_build_step
def build_python_packages(options):
    def versions_cmp(version1, version2):
        def normalize(v):
            return map(int, v.replace("-", ".").split("."))
        return cmp(normalize(version1), normalize(version2))

    def extract_version(package):
        versions = []

        output = run_captured(["apt-cache", "policy", package])
        versions_part = output.split("Version table:")[1]
        for line in versions_part.split("\n"):
            line = line.replace("***", "   ")
            if line.startswith(" " * 5) and not line.startswith(" " * 6):
                versions.append(line.split()[0])

        versions.sort(reverse=True, cmp=versions_cmp)
        if versions:
            return versions[0]
        else:
            return "0"

    retry_count = 5
    for i in xrange(retry_count):
        try:
            run(["sudo", "apt-get", "update"])
            break
        except ChildHasNonZeroExitCode:
            if i == retry_count - 1:
                raise

    for package in ["yandex-yt-python", "yandex-yt-python-tools",
                    "yandex-yt-python-yson", "yandex-yt-transfer-manager", "yandex-yt-transfer-manager-client",
                    "yandex-yt-python-fennel", "yandex-yt-local"]:
        with cwd(options.checkout_directory, "python", package):
            package_version = run_captured(
                "dpkg-parsechangelog | grep Version | awk '{print $2}'", shell=True).strip()
            uploaded_version = extract_version(package)
            teamcity_message(
                "Package {0}; package version: {1}; uploaded version: {2}".format(
                    package, package_version, uploaded_version))
            if versions_cmp(package_version, uploaded_version) <= 0:
                continue
            run(["dch", "-r", package_version, "'Resigned by teamcity'"])
        with cwd(options.checkout_directory, "python"):
            run(["./deploy.sh", package], cwd=os.path.join(options.checkout_directory, "python"))


@yt_register_build_step
def run_perl_tests(options):
    if options.build_enable_perl != "YES":
        return
    kill_by_name("^ytserver")
    run_pytest(options, "perl", "{0}/perl/tests".format(options.checkout_directory))


@yt_register_cleanup_step
def clean_artifacts(options, n=10):
    for path in ls("{0}/ARTIFACTS".format(options.working_directory),
                   reverse=True,
                   select=os.path.isfile,
                   start=n,
                   stop=sys.maxint):
        teamcity_message("Removing {0}...".format(path), status="WARNING")
        if os.path.isdir(path):
            shutil.rmtree(path, onerror=rmtree_onerror)
        else:
            os.unlink(path)


@yt_register_cleanup_step
def clean_failed_tests(options, max_allowed_size=50 * 1024 * 1024 * 1024):
    total_size = 0
    for path in ls(os.path.expanduser("~/failed_tests/"),
                   select=os.path.isdir,
                   stop=sys.maxint):
        size = os.stat(path).st_size
        if total_size + size > max_allowed_size:
            teamcity_message("Removing {0}...".format(path), status="WARNING")
            if os.path.isdir(path):
                shutil.rmtree(path, onerror=rmtree_onerror)
            else:
                os.unlink(path)
        else:
            total_size += size


################################################################################
# Below are meaty guts. Be warned.

################################################################################
################################################################################
#     *                             )                         (      ____
#   (  `          (       *   )  ( /(   (               *   ) )\ )  |   /
#   )\))(   (     )\    ` )  /(  )\())  )\ )       (  ` )  /((()/(  |  /
#  ((_)()\  )\ ((((_)(   ( )(_))((_)\  (()/(       )\  ( )(_))/(_)) | /
#  (_()((_)((_) )\ _ )\ (_(_())__ ((_)  /(_))_  _ ((_)(_(_())(_))   |/
#  |  \/  || __|(_)_\(_)|_   _|\ \ / / (_)) __|| | | ||_   _|/ __| (
#  | |\/| || _|  / _ \    | |   \ V /    | (_ || |_| |  | |  \__ \ )\
#  |_|  |_||___|/_/ \_\   |_|    |_|      \___| \___/   |_|  |___/((_)
#
################################################################################
################################################################################

################################################################################
# These methods are used to interact with TeamCity.
# See http://confluence.jetbrains.com/display/TCD8/Build+Script+Interaction+with+TeamCity

def teamcity_escape(s):
    s = re.sub("(['\\[\\]|])", "|\\1", s)
    s = s.replace("\n", "|n").replace("\r", "|r")
    s = "'" + s + "'"
    return s


def teamcity_interact(message, *args, **kwargs):

    r = " ".join(itertools.chain(
        [message],
        (
            teamcity_escape(str(x))
            for x in args
        ),
        (
            "{0}={1}".format(str(k), teamcity_escape(str(v)))
            for k, v in kwargs.iteritems())
        ))
    r = "##teamcity[" + r + "]\n"
    sys.stdout.flush()
    sys.stderr.write(r)
    sys.stderr.flush()


def teamcity_message(text, status="NORMAL"):
    if status not in ["NORMAL", "WARNING", "FAILURE", "ERROR"]:
        raise ValueError("Invalid |status|: {0}".format(status))

    if status == "NORMAL":
        teamcity_interact("message", text=text)
    else:
        teamcity_interact("message", text=text, status=status)


@contextlib.contextmanager
def teamcity_block(name):
    try:
        teamcity_interact("blockOpened", name=name)
        yield
    finally:
        teamcity_interact("blockClosed", name=name)


@contextlib.contextmanager
def teamcity_step(name, funcname):
    now = time.time()

    try:
        teamcity_message("Executing: {0}".format(name))
        with teamcity_block(name):
            yield
        teamcity_message("Completed: {0}".format(name))
    except:
        teamcity_interact(
            "message",
            text="Caught exception; failing...".format(name),
            errorDetails=traceback.format_exc(),
            status="ERROR")
        teamcity_message("Failed: {0}".format(name), status="FAILURE")
        raise
    finally:
        teamcity_interact("buildStatisticValue", key=funcname, value=int(1000.0 * (time.time() - now)))


@contextlib.contextmanager
def cwd(*args):
    old_path = os.getcwd()
    new_path = os.path.join(*args)
    try:
        teamcity_message("Changing current directory to {0}".format(new_path))
        os.chdir(new_path)
        yield
    finally:
        if os.path.exists(old_path):
            teamcity_message("Changing current directory to {0}".format(old_path))
            os.chdir(old_path)
        else:
            teamcity_message("Previous directory {0} has vanished!".format(old_path), status="WARNING")


def ls(path, reverse=True, select=None, start=0, stop=None):
    if not os.path.isdir(path):
        return []
    iterable = os.listdir(path)
    iterable = map(lambda x: os.path.realpath(os.path.join(path, x)), iterable)
    iterable = sorted(iterable, key=lambda x: os.stat(x).st_mtime, reverse=reverse)
    iterable = itertools.ifilter(select, iterable)
    iterable = itertools.islice(iterable, start, stop)
    return iterable


def mkdirp(path):
    try:
        os.makedirs(path)
    except OSError as ex:
        if ex.errno != errno.EEXIST:
            raise


_signals = dict((k, v) for v, k in signal.__dict__.iteritems() if v.startswith("SIG"))


class ChildKeepsRunningInIsolation(Exception):
    pass


class ChildHasNonZeroExitCode(Exception):
    pass


class StepFailedWithNonCriticalError(Exception):
    pass


def run_captured(*args, **kwargs):
    ignore_return_code = kwargs.get("ignore_return_code", False)
    if "ignore_return_code" in kwargs:
        del kwargs["ignore_return_code"]

    process = subprocess.Popen(*args, bufsize=1, stdout=subprocess.PIPE, **kwargs)

    output, unused_err = process.communicate()
    return_code = process.poll()
    if return_code and not ignore_return_code:
        cmd = kwargs.get("args")
        if cmd is None:
            cmd = args[0]
        error = subprocess.CalledProcessError(return_code, cmd)
        error.output = output
        raise error

    return output.strip()


def run_preexec():
    resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))


def run(args, cwd=None, env=None, silent_stdout=False, silent_stderr=False, shell=False):
    POLL_TIMEOUT = 1.0
    POLL_ITERATIONS = 5
    READ_SIZE = 4096

    with teamcity_block("({0})".format(args[0])):
        saved_env = env
        if saved_env:
            tmp = os.environ.copy()
            tmp.update(saved_env)
            env = tmp

        child = subprocess.Popen(
            args,
            bufsize=0,
            stdin=None,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            preexec_fn=run_preexec,
            close_fds=True,
            cwd=cwd,
            env=env,
            shell=shell)

        teamcity_message("run({0}) => {1}".format(
            pprint.pformat({"args": args, "cwd": cwd, "env": saved_env}),
            child.pid))

        # Since we are doing non-blocking read, we have to deal with splitting
        # by ourselves. See http://bugs.python.org/issue1175#msg56041.
        evmask_read = select.POLLIN | select.POLLPRI
        evmask_error = select.POLLHUP | select.POLLERR | select.POLLNVAL

        poller = select.poll()
        poller.register(child.stdout, evmask_read | evmask_error)
        poller.register(child.stderr, evmask_read | evmask_error)

        # Determines whether to silent any stream.
        silent_for = {
            child.stdout.fileno(): silent_stdout,
            child.stderr.fileno(): silent_stderr
        }

        # Holds the data from incomplete read()s.
        data_for = {
            child.stdout.fileno(): "",
            child.stderr.fileno(): ""
        }

        # Holds the message status.
        status_for = {
            child.stdout.fileno(): "NORMAL",
            child.stderr.fileno(): "WARNING"
        }

        # Switch FDs to non-blocking mode.
        for fd in [child.stdout.fileno(), child.stderr.fileno()]:
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

        def consume(fd, eof):
            # Try to read as much as possible from the FD.
            result = ""
            try:
                while True:
                    delta = os.read(fd, READ_SIZE)
                    if len(delta) > 0:
                        result += delta
                    else:
                        eof = True
                        break
            except OSError as e:
                if e.errno not in [errno.EAGAIN, errno.EWOULDBLOCK]:
                    raise

            # Emit complete lines from the buffer.
            data = data_for[fd] + result
            i = 0
            j = -1
            while True:
                j = data.find("\n", i)
                if j < 0:
                    break
                if not silent_for[fd]:
                    teamcity_message(data[i:j], status_for[fd])
                i = j + 1
            data_for[fd] = data[i:]

            # Emit incomplete lines from the buffer when there is no more data.
            if eof and len(data_for[fd]) > 0:
                if not silent_for[fd]:
                    teamcity_message(data_for[fd], status_for[fd])
                data_for[fd] = ""

        # Poll while we have alive FDs.
        while len(data_for) > 0:
            for fd, event in poller.poll(POLL_TIMEOUT):
                if event & evmask_read:
                    consume(fd, False)
                if event & evmask_error:
                    consume(fd, True)
                    poller.unregister(fd)
                    del data_for[fd]
                    del status_for[fd]

        # Await for the child to terminate.
        for i in xrange(POLL_ITERATIONS):
            if child.poll() is None:
                teamcity_message("Child is still running.", "WARNING")
                time.sleep(POLL_TIMEOUT)
            else:
                break

        if child.returncode is None:
            teamcity_message("Child is still running; killing it.", "WARNING")
            child.kill()
            raise ChildKeepsRunningInIsolation()

        if child.returncode < 0:
            teamcity_message(
                "Child was terminated by signal {0}".format(_signals[-child.returncode]),
                "FAILURE")

        if child.returncode > 0:
            teamcity_message(
                "Child has exited with return code {0}".format(child.returncode),
                "FAILURE")

        if child.returncode == 0:
            teamcity_message("Child has exited successfully")
        else:
            raise ChildHasNonZeroExitCode(str(child.returncode))


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
        "--cc",
        type=str, action="store", required=False, default="gcc-4.8")
    parser.add_argument(
        "--cxx",
        type=str, action="store", required=False, default="g++-4.8")

    options = parser.parse_args()
    # XXX(asaitgalin): parallel testing is enabled only for bare metal machines.
    options.enable_parallel_testing = socket.getfqdn().endswith("tc.yt.yandex.net")

    status = 0
    try:
        for step in _build_steps:
            try:
                with teamcity_step("Build Step '{0}'".format(step.func_name), step.func_name):
                    step(options)
            except StepFailedWithNonCriticalError as err:
                teamcity_message(err)
                status = 42
    except:
        teamcity_message("Terminating...", status="FAILURE")
        status = 43
    finally:
        for step in _cleanup_steps:
            try:
                with teamcity_step("Clean-up Step '{0}'".format(step.func_name), step.func_name):
                    step(options)
            except:
                teamcity_interact(
                    "message",
                    text="Caught exception during cleanup phase; ignoring...",
                    errorDetails=traceback.format_exc(),
                    status="ERROR")
        teamcity_message("Done!")
        sys.exit(status)


if __name__ == "__main__":
    main()
