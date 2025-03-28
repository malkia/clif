"""Repository rule for Python autoconfiguration.

`python_configure` depends on the following environment variables:

  * `PYTHON_BIN_PATH`: location of python binary.
  * `PYTHON_LIB_PATH`: Location of python libraries.

This file is a modified version of
//third_party/py/riegeli/python_configure.bzl with following changes:

  * Removed numpy related rules and functions
  * Added rule for symlinking libpython.so
  * Default to looking for python binary using python3 command first
"""

_BAZEL_SH = "BAZEL_SH"
_PYTHON_BIN_PATH = "PYTHON_BIN_PATH"
_PYTHON_LIB_PATH = "PYTHON_LIB_PATH"
_TF_PYTHON_CONFIG_REPO = "TF_PYTHON_CONFIG_REPO"

def _tpl(repository_ctx, tpl, substitutions = {}, out = None):
    if not out:
        out = tpl
    repository_ctx.template(
        out,
        Label("//third_party/py:{}.tpl".format(tpl)),
        substitutions,
    )

def _fail(msg):
    """Outputs failure message when auto configuration fails."""
    red = "\033[0;31m"
    no_color = "\033[0m"
    fail("{}Python Configuration Error:{} {}\n".format(red, no_color, msg))

def _is_windows(repository_ctx):
    """Returns true if the host operating system is Windows."""
    os_name = repository_ctx.os.name.lower()
    return "windows" in os_name

def _execute(
        repository_ctx,
        cmdline,
        error_msg = None,
        error_details = None,
        empty_stdout_fine = False):
    """Executes an arbitrary shell command.

    Args:
      repository_ctx: the repository_ctx object
      cmdline: list of strings, the command to execute
      error_msg: string, a summary of the error if the command fails
      error_details: string, details about the error or steps to fix it
      empty_stdout_fine: bool, if True, an empty stdout result is fine,
        otherwise it's an error
    Return:
      the result of repository_ctx.execute(cmdline)
    """
    result = repository_ctx.execute(cmdline)
    if result.stderr or not (empty_stdout_fine or result.stdout):
        _fail("\n".join([
            error_msg.strip() if error_msg else "Repository command failed",
            result.stderr.strip(),
            error_details if error_details else "",
        ]))
    return result

def _read_dir(repository_ctx, src_dir):
    """Returns a string with all files in a directory.

    Finds all files inside a directory, traversing subfolders and following
    symlinks. The returned string contains the full path of all files
    separated by line breaks.
    """
    if _is_windows(repository_ctx):
        src_dir = src_dir.replace("/", "\\")
        find_result = _execute(
            repository_ctx,
            ["cmd.exe", "/c", "dir", src_dir, "/b", "/s", "/a-d"],
            empty_stdout_fine = True,
        )

        # src_files will be used in genrule.outs where the paths must
        # use forward slashes.
        result = find_result.stdout.replace("\\", "/")
    else:
        find_result = _execute(
            repository_ctx,
            ["find", src_dir, "-follow", "-type", "f"],
            empty_stdout_fine = True,
        )
        result = find_result.stdout
    return result

def _genrule(src_dir, genrule_name, command, outs):
    """Returns a string with a genrule.

    Genrule executes the given command and produces the given outputs.
    """
    return (
        "genrule(\n" +
        '    name = "{}",\n' +
        "    outs = [\n" +
        "{}\n" +
        "    ],\n" +
        '    cmd = """\n' +
        "{}\n" +
        '   """,\n' +
        ")\n"
    ).format(genrule_name, outs, command)

def _norm_path(path):
    """Returns a path with '/' and removes the trailing slash."""
    return path.replace("\\", "/").rstrip("/")

def _symlink_genrule_for_dir(
        repository_ctx,
        src_dir,
        dest_dir,
        genrule_name,
        src_files = [],
        dest_files = []):
    """Returns a genrule to symlink (or copy if on Windows) a set of files.

    If src_dir is passed, files will be read from the given directory; otherwise
    we assume files are in src_files and dest_files
    """
    if src_dir != None:
        src_dir = _norm_path(src_dir)
        dest_dir = _norm_path(dest_dir)
        files = "\n".join(
            sorted(_read_dir(repository_ctx, src_dir).splitlines()),
        )

        # Create a list with the src_dir stripped to use for outputs.
        dest_files = files.replace(src_dir, "").splitlines()
        src_files = files.splitlines()
    command = []
    outs = []
    for i in range(len(dest_files)):
        if dest_files[i] != "":
            # If we have only one file to link we do not want to use the
            # dest_dir, as $(@D) will include the full path to the file.
            dest = "$(@D)/{}{}".format(
                dest_dir if len(dest_files) != 1 else "",
                dest_files[i],
            )

            # Copy the headers to create a sandboxable setup.
            cmd = "cp -f"
            command.append('{} "{}" "{}"'.format(cmd, src_files[i], dest))
            outs.append('        "{}{}",'.format(dest_dir, dest_files[i]))
    genrule = _genrule(
        src_dir,
        genrule_name,
        " && ".join(command),
        "\n".join(outs),
    )
    return genrule

def _get_python_bin(repository_ctx):
    """Gets the python bin path."""
    python_bin = repository_ctx.os.environ.get(_PYTHON_BIN_PATH)
    if python_bin != None:
        return python_bin

    # First check for an explicit "python3"
    python_bin = repository_ctx.which("python3")
    if python_bin != None:
        return python_bin

    # Some systems just call pythone3 "python"
    python_bin = repository_ctx.which("python")
    if python_bin != None:
        return python_bin

    _fail(("Cannot find python in PATH, please make sure " +
           "python is installed and add its directory in PATH, " +
           "or --define {}='/something/else'.\nPATH={}").format(
        _PYTHON_BIN_PATH,
        repository_ctx.os.environ.get("PATH", ""),
    ))
    return python_bin  # unreachable

def _get_bash_bin(repository_ctx):
    """Gets the bash bin path."""
    bash_bin = repository_ctx.os.environ.get(_BAZEL_SH)
    if bash_bin != None:
        return bash_bin
    bash_bin_path = repository_ctx.which("bash")
    if bash_bin_path != None:
        return str(bash_bin_path)
    _fail(("Cannot find bash in PATH, please make sure " +
           "bash is installed and add its directory in PATH, " +
           "or --define {}='/path/to/bash'.\nPATH={}").format(
        _BAZEL_SH,
        repository_ctx.os.environ.get("PATH", ""),
    ))
    return bash_bin  # unreachable

def _get_python_runtime_pair(repository_ctx, python_bin):
    """Builds a py_runtime_pair definition."""
    return (
        "py_runtime_pair(\n" +
        '    name = "py_runtime_pair",\n' +
        "    py2_runtime = None,\n" +
        "    py3_runtime = \":py3_runtime\",\n" +
        ")\n" +
        "\n" +
        "py_runtime(\n" +
        '    name = "py3_runtime",\n' +
        '    interpreter_path = "{}",\n' +
        '    python_version = "PY3",\n' +
        ")\n"
    ).format(python_bin)

def _get_python_lib(repository_ctx, python_bin):
    """Gets the python lib path."""
    python_lib = repository_ctx.os.environ.get(_PYTHON_LIB_PATH)
    if python_lib != None:
        return python_lib
    print_lib = ("<<END\n" +
                 "import site\n" +
                 "import os\n" +
                 "\n" +
                 "try:\n" +
                 "  input = raw_input\n" +
                 "except NameError:\n" +
                 "  pass\n" +
                 "\n" +
                 "python_paths = []\n" +
                 "if os.getenv('PYTHONPATH') is not None:\n" +
                 "  python_paths = os.getenv('PYTHONPATH').split(':')\n" +
                 "try:\n" +
                 "  library_paths = site.getsitepackages()\n" +
                 "except AttributeError:\n" +
                 "  from distutils.sysconfig import get_python_lib\n" +
                 "  library_paths = [get_python_lib()]\n" +
                 "all_paths = set(python_paths + library_paths)\n" +
                 "paths = []\n" +
                 "for path in all_paths:\n" +
                 "  if os.path.isdir(path):\n" +
                 "    paths.append(path)\n" +
                 "if len(paths) >= 1:\n" +
                 "  print(paths[0])\n" +
                 "END")
    cmd = "{} - {}".format(python_bin, print_lib)
    result = repository_ctx.execute([_get_bash_bin(repository_ctx), "-c", cmd])
    return result.stdout.strip("\n")

def _check_python_lib(repository_ctx, python_lib):
    """Checks the python lib path."""
    cmd = 'test -d "{}" -a -x "{}"'.format(python_lib, python_lib)
    result = repository_ctx.execute([_get_bash_bin(repository_ctx), "-c", cmd])
    if result.return_code == 1:
        _fail("Invalid python library path: {}".format(python_lib))

def _check_python_bin(repository_ctx, python_bin):
    """Checks the python bin path."""
    cmd = '[[ -x "{}" ]] && [[ ! -d "{}" ]]'.format(python_bin, python_bin)
    result = repository_ctx.execute([_get_bash_bin(repository_ctx), "-c", cmd])
    if result.return_code == 1:
        _fail(("--define {}='{}' is not executable. " +
               "Is it the python binary?").format(
            _PYTHON_BIN_PATH,
            python_bin,
        ))

def _get_python_include(repository_ctx, python_bin):
    """Gets the python include path."""
    result = _execute(
        repository_ctx,
        [
            python_bin,
            "-c",
            "from distutils import sysconfig; " +
            "print(sysconfig.get_python_inc())",
        ],
        error_msg = "Problem getting python include path.",
        error_details = ("Is the Python binary path set up right? " +
                         "(See {}.) " +
                         "Is distutils installed?").format(_PYTHON_BIN_PATH),
    )
    return result.stdout.splitlines()[0]

def _get_python_import_lib_names(repository_ctx, python_bin):
    """Gets Python import library names (pythonXY.lib and pythonXY_d.lib) on Windows."""
    result = _execute(
        repository_ctx,
        [
            python_bin,
            "-c",
            "import sys; " +
            'print("python{}{}.lib".format(' +
            "sys.version_info.major, sys.version_info.minor)); " +
            'print("python{}{}_d.lib".format(' +
            "sys.version_info.major, sys.version_info.minor))"
        ],
        error_msg = "Problem getting python import library.",
        error_details = ("Is the Python binary path set up right? " +
                         "(See {}.) ").format(_PYTHON_BIN_PATH),
    )
    return result.stdout.splitlines()

def _get_python_ldlibrary(repository_ctx, python_bin):
    """Get libpython.so path."""
    result = _execute(
        repository_ctx,
        [
            python_bin,
            "-c",
            "import os; from distutils.sysconfig import get_config_var;" +
            "print(os.path.join(get_config_var('LIBPL'), get_config_var('LDLIBRARY')))",
        ],
        error_msg = "Problem getting python import library.",
        error_details = ("Is the Python binary path set up right? " +
                         "(See {}.) ").format(_PYTHON_BIN_PATH),
    )
    return result.stdout.splitlines()[0]

def _create_local_python_repository(repository_ctx):
    """Creates the repository containing files set up to build with Python."""
    python_bin = _get_python_bin(repository_ctx)
    _check_python_bin(repository_ctx, python_bin)
    python_runtime_pair = _get_python_runtime_pair(repository_ctx, python_bin)
    python_lib = _get_python_lib(repository_ctx, python_bin)
    _check_python_lib(repository_ctx, python_lib)
    python_include = _get_python_include(repository_ctx, python_bin)
    python_include_rule = _symlink_genrule_for_dir(
        repository_ctx,
        python_include,
        "python_include",
        "python_include",
    )
    python_import_lib_genrule = ""
    python_import_lib_windows_dbg_genrule = ""
    python_import_lib_src = ""
    python_import_lib_name = ""

    # To build Python C/C++ extension on Windows, we need to link to python
    # import library pythonXY.lib or pythonXY_d.lib
    # See https://docs.python.org/3/extending/windows.html
    if _is_windows(repository_ctx):
        python_include = _norm_path(python_include)
        python_import_lib_names = _get_python_import_lib_names(
            repository_ctx,
            python_bin,
        )
        python_root = python_include.rsplit("/", 1)[0]
        python_import_lib_srcs = [
            "{}/libs/{}".format(python_root, python_import_lib_names[0]),
            "{}/libs/{}".format(python_root, python_import_lib_names[1]),
        ]
        python_import_lib_src = python_import_lib_srcs[0]
        python_import_lib_name = python_import_lib_names[0]
        python_import_lib_windows_dbg_genrule = _symlink_genrule_for_dir(
            repository_ctx,
            None,
            "",
            "python_import_lib_windows_dbg",
            [python_import_lib_srcs[1]],
            [python_import_lib_names[1]],
        )
    else:
        python_import_lib_src = _get_python_ldlibrary(repository_ctx, python_bin)
        python_import_lib_name = python_import_lib_src.rsplit("/", 1)[1]
    python_import_lib_genrule = _symlink_genrule_for_dir(
        repository_ctx,
        None,
        "",
        "python_import_lib",
        [python_import_lib_src],
        [python_import_lib_name],
    )
    _tpl(repository_ctx, "BUILD", {
        "%{PYTHON_RUNTIME_PAIR}": python_runtime_pair,
        "%{PYTHON_INCLUDE_GENRULE}": python_include_rule,
        "%{PYTHON_IMPORT_LIB_GENRULE}": python_import_lib_genrule,
        "%{PYTHON_IMPORT_LIB_WINDOWS_DBG_GENRULE}": python_import_lib_windows_dbg_genrule,
    })

def _create_remote_python_repository(repository_ctx, remote_config_repo):
    """Creates pointers to a remotely configured repo set up to build with Python.
    """
    repository_ctx.template("BUILD", Label(remote_config_repo + ":BUILD"), {})

def _python_autoconf_impl(repository_ctx):
    """Implementation of the python_autoconf repository rule."""
    if _TF_PYTHON_CONFIG_REPO in repository_ctx.os.environ:
        _create_remote_python_repository(
            repository_ctx,
            repository_ctx.os.environ[_TF_PYTHON_CONFIG_REPO],
        )
    else:
        _create_local_python_repository(repository_ctx)

python_configure = repository_rule(
    implementation = _python_autoconf_impl,
    environ = [
        _BAZEL_SH,
        _PYTHON_BIN_PATH,
        _PYTHON_LIB_PATH,
        _TF_PYTHON_CONFIG_REPO,
    ],
)
"""Detects and configures the local Python.

Add the following to your WORKSPACE FILE:

```python
python_configure(name = "local_config_python")
```

Args:
  name: A unique name for this workspace rule.
"""
