#!/usr/bin/env python

from yt.common import makedirp
from yt.wrapper.common import MB, parse_bool, chunk_iter_stream
from yt.wrapper.cli_helpers import run_main
import yt.logger as logger
import yt.yson as yson
import yt.wrapper as yt

import os
import argparse
import subprocess
import shutil
from functools import partial

SCRIPT_DIR = os.path.dirname(__file__)

def preexec_dup2(new_fds, output_path):
    for new_fd in new_fds:
        fd = os.open(os.path.join(output_path, str(new_fd)), os.O_CREAT | os.O_WRONLY)
        os.dup2(fd, new_fd)
        if fd != new_fd:
            os.close(fd)

def main():
    parser = argparse.ArgumentParser(description="Job runner for yt-job-tool")
    parser.add_argument("--config-path", help="path to YSON config")
    args = parser.parse_args()

    if args.config_path is None:
        args.config_path = os.path.join(SCRIPT_DIR, "run_config")

    with open(args.config_path, "rb") as f:
        config = yson.load(f)

    with open(config["command_path"], "rb") as fin:
        command = fin.read()

    use_yamr_descriptors = parse_bool(config["use_yamr_descriptors"])
    if use_yamr_descriptors:
        new_fds = [1, 2] + [i + 3 for i in xrange(config["output_table_count"])]
    else:
        new_fds = [2] + [3 * i + 1 for i in xrange(config["output_table_count"])]

    if not use_yamr_descriptors:
        new_fds.append(5)  # Job statistics are written to fifth descriptor

    makedirp(config["output_path"])

    env = {
        "YT_JOB_INDEX": "0",
        "YT_OPERATION_ID": config["operation_id"],
        "YT_JOB_ID": config["job_id"],
        "YT_STARTED_BY_JOB_TOOL": "1"
    }
    process = subprocess.Popen(command, shell=True, close_fds=False, stdin=subprocess.PIPE,
                               preexec_fn=partial(preexec_dup2, new_fds=new_fds, output_path=config["output_path"]),
                               cwd=config["sandbox_path"], env=env)
    logger.info("Started job process")

    with open(config["input_path"], "rb") as fin:
        for chunk in chunk_iter_stream(fin, 16 * MB):
            if not chunk:
                break
            process.stdin.write(chunk)
    process.stdin.close()
    process.wait()

    if process.returncode != 0:
        with open(os.path.join(config["output_path"], "2"), "rb") as f:
            stderr = f.read()
        raise yt.YtError("User job exited with non-zero exit code {0} with stderr:\n{1}"
                         .format(process.returncode, stderr))

    logger.info("Job process exited successfully. Job output (file names correspond to file descriptors) "
                "can be found in %s", config["output_path"])

def make_run_script(destination_dir):
    path = os.path.realpath(__file__)
    if path.endswith(".pyc"):
        path = path[:-1]
    shutil.copy2(path, os.path.join(destination_dir, "run"))

if __name__ == "__main__":
    run_main(main)
