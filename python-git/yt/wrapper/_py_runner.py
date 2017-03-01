def get_platform_version():
    import sys
    import platform

    version = []
    name = sys.platform
    version.append(name)
    if name in ("linux", "linux2"):
        version.append(platform.linux_distribution())
    return tuple(version)

def main():
    # We should use local imports because of replacing __main__ module cause cleaning globals.
    import os
    import sys
    import imp
    import time
    import zipfile
    import pickle as standard_pickle

    start_time = time.time()

    # Variable names start with "__" to avoid accidental intersection with scope of user function.
    __operation_dump_filename = sys.argv[1]
    __config_dump_filename = sys.argv[2]

    if len(sys.argv) > 3:
        __modules_info_filename = sys.argv[3]
        __main_filename = sys.argv[4]
        __main_module_name = sys.argv[5]
        __main_module_type = sys.argv[6]

        with open(__modules_info_filename, "rb") as fin:
            modules_info = standard_pickle.load(fin)

        __python_eggs = []
        for info in modules_info["modules"]:
            destination = "modules"
            if info.get("tmpfs") and os.path.exists("tmpfs"):
                destination = "tmpfs/modules"

            # Python eggs which will be added to sys.path
            eggs = info.get("eggs")
            if eggs is not None:
                __python_eggs.extend([os.path.join(".", destination, egg) for egg in eggs])

            # Unfortunately we cannot use fixed version of ZipFile.
            __zip = zipfile.ZipFile(info["filename"])
            __zip.extractall(destination)
            __zip.close()

        module_locations = ["./modules", "./tmpfs/modules"]
        sys.path = __python_eggs + module_locations + sys.path

        client_version = modules_info["platform_version"]
        server_version = get_platform_version()

        if client_version is not None and client_version != server_version:
            from shutil import rmtree
            for location in module_locations:
                yson_bindings_path = os.path.join(location, "yt_yson_bindings")
                if os.path.exists(yson_bindings_path):
                    rmtree(yson_bindings_path)

        if "." in __main_module_name:
            __main_module_package = __main_module_name.rsplit(".", 1)[0]
            __import__(__main_module_package)
        main_module = imp.load_module(__main_module_name,
                                      open(__main_filename, 'rb'),
                                      __main_filename,
                                      ('', 'rb', imp.__dict__[__main_module_type]))

        main_module_dict = globals()
        if "__main__" in sys.modules:
            main_module_dict = sys.modules["__main__"].__dict__
        for name in dir(main_module):
            main_module_dict[name] = main_module.__dict__[name]


    import yt.wrapper
    yt.wrapper.py_runner_helpers.process_rows(__operation_dump_filename, __config_dump_filename, start_time=start_time)

if __name__ == "__main__":
    main()

