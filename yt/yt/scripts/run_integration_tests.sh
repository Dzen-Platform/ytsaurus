#!/bin/bash -eu

export SOURCE_ROOT="$(pwd)/ytsaurus"
export BUILD_ROOT="$(realpath ../build)"
export PYTHON_ROOT="$(realpath ../python)"
export VIRTUALENV_PATH="$(realpath ../venv)"
export TESTS_SANDBOX="$(realpath ../tests_sandbox)"

source "$VIRTUALENV_PATH/bin/activate"

cd $SOURCE_ROOT/yt/yt/tests/integration

./run_tests.sh -m opensource
