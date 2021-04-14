PACKAGE_NAME = "yandex-yt-local"

def main():
    from helpers import prepare_files, get_version

    from setuptools import setup
    from setuptools.command.test import test as TestCommand

    import sys

    class PyTest(TestCommand):
        def finalize_options(self):
            TestCommand.finalize_options(self)
            self.test_args = ["-vs"]
            self.test_suite = True

        def run_tests(self):
            # import here, cause outside the eggs aren't loaded
            import pytest
            pytest.main(self.test_args)

    requires = []
    if sys.version_info[:2] <= (2, 6):
        requires.append("argparse")
    requires.append("yandex-yt>=0.10.11")

    scripts, data_files = prepare_files([
        "yt/local/bin/yt_local",
        "yt/environment/bin/yt_env_watcher"
    ])

    setup(
        name=PACKAGE_NAME,
        # To avoid 'post' in version.
        version=get_version().split("-")[0],
        packages=["yt.local", "yt.environment", "yt.environment.api", "yt.test_helpers"],
        scripts=scripts,

        install_requires=requires,

        author="Andrey Saitgalin",
        author_email="asaitgalin@yandex-team.ru",
        description="Python libraries and CLI to manage local YT instances.",
        keywords="yt local mapreduce",

        long_description=\
            "This package contains python library which helps to set up " \
            "fully-functional YT cluster instance locally. It is designed to be " \
            "flexible and allows to choose desired YT cluster configuration. " \
            "Also this packages provides yt_local binary (based on python library) " \
            "which can be used to manage local YT instances manually from command-line.",

        # Using py.test, because it much more verbose
        cmdclass={"test": PyTest},
        tests_require=["pytest"],

        data_files=data_files
    )

if __name__ == "__main__":
    main()
