load("@bazel_tools//tools/python:toolchain.bzl", "py_runtime_pair")

licenses(["restricted"])

package(default_visibility = ["//visibility:public"])

toolchain(
    name = "toolchain",
    toolchain = ":py_runtime_pair",
    toolchain_type = "@bazel_tools//tools/python:toolchain_type",
)

# To build Python C/C++ extension on Windows, we need to link to python import library pythonXY.lib
# See https://docs.python.org/3/extending/windows.html
cc_import(
    name = "python_lib",
    interface_library = select({
        ":windows_dbg": ":python_import_lib_windows_dbg",
        "//conditions:default": "python_import_lib"
    }),
    system_provided = 1,
)

cc_library(
    name = "python_headers",
    hdrs = [":python_include"],
    deps = [":python_lib"],
    includes = ["python_include"],
)

config_setting(
    name = "windows",
    values = {"cpu": "x64_windows"},
)

config_setting(
    name = "windows_dbg",
    values = {
        "cpu": "x64_windows", 
        "compilation_mode": "dbg"
    },
)

%{PYTHON_RUNTIME_PAIR}
%{PYTHON_INCLUDE_GENRULE}
%{PYTHON_IMPORT_LIB_GENRULE}
%{PYTHON_IMPORT_LIB_WINDOWS_DBG_GENRULE}
