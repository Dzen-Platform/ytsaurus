from helpers import get_version, prepare_files

from setuptools import setup

def main():
    requires = ["yandex-yt", "yandex-yt-tools", "yandex-yt-yson-bindings", "flask", "python-prctl"]
    scripts, data_files = prepare_files(["yt/transfer_manager/server/bin/transfer-manager-server"])

    setup(
        name = "yandex-yt-transfer-manager",
        version = get_version(),
        packages = ["yt.transfer_manager", "yt.transfer_manager.server"],

        scripts = scripts,
        data_files = data_files,

        install_requires = requires,

        author = "Ignat Kolesnichenko",
        author_email = "ignat@yandex-team.ru",
        description = "Daemon that provides scheduler to copy tables between YT and Yamr clusters",
        keywords = "yt python transfer import export mapreduce",
    )

if __name__ == "__main__":
    main()
