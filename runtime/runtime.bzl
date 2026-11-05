"""Source-built, relocatable Cyber runtime contributions and products."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_pkg//pkg:providers.bzl", "PackageFilegroupInfo", "PackageFilesInfo")

RuntimeModuleInfo = provider(
    doc = "A namespaced module contribution, independent of its final product.",
    fields = {
        "name": "Stable module name.",
        "entries": "List of (File, relative installation destination) pairs.",
        "libraries": "Declared transitive shared libraries.",
        "executables": "Module executable destinations requiring environment wrappers.",
    },
)

RuntimeResourcesInfo = provider(
    doc = "Namespaced resource files contributed by one module.",
    fields = {
        "module_name": "Owning module.",
        "entries": "List of (File, destination) pairs.",
    },
)

RuntimeLayoutInfo = provider(
    doc = "One complete, relocatable runtime installation.",
    fields = {
        "root": "Materialized runtime tree artifact.",
        "modes": "Executable keys captured before Bazel normalizes tree modes.",
    },
)

def _safe_key(path):
    if not path or path.startswith("/") or "\\" in path:
        fail("unsafe runtime destination: " + repr(path))
    for part in (path[:-1] if path.endswith("/") else path).split("/"):
        if part in ["", ".", ".."]:
            fail("unsafe runtime destination: " + repr(path))
    return path

def _module_name(name):
    _safe_key(name)
    if "/" in name:
        fail("module_name must be one path segment: " + name)

def _owned(name, destination):
    _safe_key(destination)
    if destination.startswith("share/core/"):
        fail("reserved core runtime destination: " + destination)
    prefixes = [
        "conf/" + name + "/",
        "dag/" + name + "/",
        "launch/" + name + "/",
        "share/" + name + "/",
        "lib/components/" + name + "/",
        "bin/modules/" + name + "/",
        "assets/" + name + "/",
        "assets/models/" + name + "/",
        "models/" + name + "/",  # Explicit compatibility with older model keys.
    ]
    if not any([destination.startswith(prefix) for prefix in prefixes]):
        fail("module %s cannot own %s; shared Cyber configuration is owned by runtime_layout" % (name, destination))
    if destination in ["conf/" + name + "/cyber.pb.conf"]:
        fail("shared Cyber configuration is owned by runtime_layout")

def _mapped_files(mapping):
    entries = []
    for target, destination in mapping.items():
        _safe_key(destination)
        files = target[DefaultInfo].files.to_list()
        if not files and not destination.endswith("/"):
            fail("required runtime input is empty: " + str(target.label))
        if not destination.endswith("/") and len(files) != 1:
            fail("exact runtime destination requires one file: " + destination)
        for file in files:
            if file.is_directory:
                path = destination.rstrip("/") + "/"
            else:
                path = destination + file.basename if destination.endswith("/") else destination
            entries.append((file, path))
    return entries

def _runfile_path(path, workspace, root = False):
    if path.startswith("../"):
        key = path[3:]
    else:
        key = path if root else workspace + "/" + path
    return _safe_key(key)

def _runfile_key(file, workspace):
    return _runfile_path(file.short_path, workspace)

def _runfiles(target, prefix, workspace, empty):
    entries = []
    runfiles = target[DefaultInfo].default_runfiles
    if runfiles:
        for file in runfiles.files.to_list():
            entries.append((file, prefix + "/" + _runfile_key(file, workspace)))
        for link in runfiles.symlinks.to_list():
            entries.append((link.target_file, prefix + "/" + _runfile_path(link.path, workspace)))
        for link in runfiles.root_symlinks.to_list():
            entries.append((link.target_file, prefix + "/" + _runfile_path(link.path, workspace, root = True)))
        for path in runfiles.empty_filenames.to_list():
            entries.append((empty, prefix + "/" + _runfile_path(path, workspace)))
    mapping = target[DefaultInfo].files_to_run.repo_mapping_manifest
    if mapping:
        entries.append((mapping, prefix + "/_repo_mapping"))
    return entries

def _libraries(targets):
    libraries = []
    for target in targets:
        if CcInfo in target:
            for linker_input in target[CcInfo].linking_context.linker_inputs.to_list():
                for library in linker_input.libraries:
                    for file in [library.dynamic_library, library.resolved_symlink_dynamic_library]:
                        if file:
                            libraries.append(file)
        info = target[DefaultInfo]
        files = info.files.to_list()
        if info.default_runfiles:
            files += info.default_runfiles.files.to_list()
        for file in files:
            if ".so" in file.basename or file.basename.endswith(".dylib"):
                libraries.append(file)
    return depset(libraries)

def _module_impl(ctx):
    name = ctx.attr.module_name
    _module_name(name)
    entries = _mapped_files(ctx.attr.files)
    executables = []
    for target in ctx.attr.components:
        for file in target[DefaultInfo].files.to_list():
            entries.append((file, "lib/components/%s/%s" % (name, file.basename)))
        entries += _runfiles(target, "share/" + name + "/runfiles", ctx.workspace_name, ctx.file._empty_file)
    for target in ctx.attr.executables:
        executable = target[DefaultInfo].files_to_run.executable
        if not executable:
            fail("runtime executable target has no executable: " + str(target.label))
        destination = "bin/modules/%s/%s" % (name, executable.basename)
        entries.append((executable, destination))
        executables.append(destination)
        entries += _runfiles(target, destination + ".runfiles", ctx.workspace_name, ctx.file._empty_file)
    for target in ctx.attr.resources:
        info = target[RuntimeResourcesInfo]
        if info.module_name != name:
            fail("runtime resources belong to %s, not %s" % (info.module_name, name))
        entries += info.entries
    for target in ctx.attr.dependency_metadata:
        for file in target[DefaultInfo].files.to_list():
            entries.append((file, "share/%s/dependencies/%s" % (name, file.basename)))
    for _, destination in entries:
        _owned(name, destination)
    libraries = _libraries(ctx.attr.components + ctx.attr.executables)
    return [
        RuntimeModuleInfo(name = name, entries = entries, libraries = libraries, executables = executables),
        DefaultInfo(files = depset([file for file, _ in entries], transitive = [libraries])),
    ]

runtime_module = rule(
    implementation = _module_impl,
    attrs = {
        "module_name": attr.string(mandatory = True),
        "components": attr.label_list(allow_files = True),
        "executables": attr.label_list(),
        "files": attr.label_keyed_string_dict(allow_files = True),
        "resources": attr.label_list(providers = [RuntimeResourcesInfo]),
        "dependency_metadata": attr.label_list(allow_files = True),
        "_empty_file": attr.label(default = Label("//runtime:empty"), allow_single_file = True),
    },
)

def _resources_impl(ctx):
    name = ctx.attr.module_name
    _module_name(name)
    namespace = ctx.attr.namespace or name
    _safe_key(namespace)
    if namespace != name and not namespace.startswith(name + "/"):
        fail("runtime resource namespace must belong to module " + name)
    entries = []
    for file in ctx.files.files:
        path = file.short_path
        parts = path.split("/")[2:] if path.startswith("../") else path.split("/")
        category = ""
        tail = file.basename
        for index, part in enumerate(parts):
            if part in ["conf", "dag", "launch", "models", "assets", "tools", "share"]:
                category = part
                tail = "/".join(parts[index + 1:])
                break
        if category == "assets" and tail.startswith("models/"):
            category = "models"
            tail = tail[len("models/"):]
        if category == "models":
            entries.append((file, "assets/models/" + namespace + "/" + tail))
            if ctx.attr.legacy_models:
                entries.append((file, "models/" + namespace + "/" + tail))
        elif category in ["conf", "dag", "launch", "share"]:
            entries.append((file, category + "/" + namespace + "/" + tail))
        elif category == "tools":
            entries.append((file, "bin/modules/" + namespace + "/" + tail))
        else:
            entries.append((file, "assets/" + namespace + "/" + tail))
    for _, destination in entries:
        _owned(name, destination)
    return [
        RuntimeResourcesInfo(module_name = name, entries = entries),
        DefaultInfo(files = depset(ctx.files.files)),
    ]

runtime_resources = rule(
    implementation = _resources_impl,
    attrs = {
        "module_name": attr.string(mandatory = True),
        "namespace": attr.string(),
        "files": attr.label_list(allow_files = True),
        "legacy_models": attr.bool(default = True),
    },
)

def _layout_impl(ctx):
    if ctx.attr.default_dag and ctx.attr.default_launch:
        fail("default_dag and default_launch are mutually exclusive")
    for value in [ctx.attr.default_dag, ctx.attr.default_launch]:
        if value:
            _safe_key(value)
    for key in ctx.attr.required_assets.keys() + ctx.attr.required_configs:
        _safe_key(key)
        if key.endswith("/"):
            fail("required runtime key must name a file: " + key)
    for digest in ctx.attr.required_assets.values():
        if len(digest) != 64 or any([char not in "0123456789abcdefABCDEF" for char in digest.elems()]):
            fail("required asset digest must be SHA-256 hex: " + digest)
    modules = []
    entries = _mapped_files(ctx.attr.files)
    for _, destination in entries:
        if destination.startswith("bin/") or destination == "conf/runtime.json" or destination.startswith("share/core/"):
            fail("reserved core runtime destination: " + destination)
    libraries = []
    executables = []
    for target in ctx.attr.modules:
        info = target[RuntimeModuleInfo]
        if info.name in modules:
            fail("duplicate runtime module name: " + info.name)
        modules.append(info.name)
        entries += info.entries
        libraries.append(info.libraries)
        executables += info.executables
    for file in ctx.files.cyber_conf:
        entries.append((file, "conf/" + file.basename))
    tool_targets = ctx.attr.tools.keys()
    for target, destination in ctx.attr.tools.items():
        _safe_key(destination)
        executable = target[DefaultInfo].files_to_run.executable
        files = target[DefaultInfo].files.to_list()
        if not executable and len(files) != 1:
            fail("core runtime tool requires one executable or file: " + str(target.label))
        entries.append((executable or files[0], destination))
        entries += _runfiles(target, destination + ".runfiles", ctx.workspace_name, ctx.file._empty_file)
    for target in ctx.attr.python_runtime:
        entries += _runfiles(target, "python", ctx.workspace_name, ctx.file._empty_file)

        # Also provide the public wheel spelling without requiring a wheel build.
        for file in target[DefaultInfo].default_runfiles.files.to_list():
            path = file.short_path
            if "/cyber_py3/" in path:
                entries.append((file, "python/pycyber/" + path.split("/cyber_py3/")[1]))
            elif "/python/internal/" in path and file.basename.endswith(".so"):
                entries.append((file, "python/pycyber/internal/" + file.basename))
            elif "/cyber/proto/" in "/" + path and file.basename.endswith("_pb2.py"):
                entries.append((file, "python/pycyber/proto/" + file.basename))
        libraries.append(_libraries([target]))
    libraries.append(_libraries(tool_targets))
    library_files = depset(transitive = libraries).to_list()
    root = ctx.actions.declare_directory(ctx.label.name)
    modes = ctx.actions.declare_file(ctx.label.name + ".modes.json")
    manifest = ctx.actions.declare_file(ctx.label.name + ".manifest.json")
    ctx.actions.write(manifest, json.encode({
        "files": [[file.path, destination] for file, destination in entries],
        "libraries": [file.path for file in library_files],
        "modules": modules,
        "default_dag": ctx.attr.default_dag,
        "default_launch": ctx.attr.default_launch,
        "required_assets": ctx.attr.required_assets,
        "required_configs": ctx.attr.required_configs,
        "executables": executables,
    }))
    ctx.actions.run(
        executable = ctx.executable._materializer,
        arguments = ["--modes", modes.path, manifest.path, root.path],
        inputs = depset([manifest] + [file for file, _ in entries] + library_files),
        tools = [ctx.attr._materializer[DefaultInfo].files_to_run],
        outputs = [root, modes],
        mnemonic = "RuntimeLayout",
        progress_message = "Assembling unified runtime %{label}",
    )
    return [
        RuntimeLayoutInfo(root = root, modes = modes),
        DefaultInfo(files = depset([root]), runfiles = ctx.runfiles(files = [root])),
        OutputGroupInfo(manifest = depset([manifest])),
    ]

runtime_layout = rule(
    implementation = _layout_impl,
    attrs = {
        "modules": attr.label_list(providers = [RuntimeModuleInfo]),
        "files": attr.label_keyed_string_dict(allow_files = True),
        "cyber_conf": attr.label(default = Label("//cyber:cyber_conf"), allow_files = True),
        "default_dag": attr.string(),
        "default_launch": attr.string(),
        "required_assets": attr.string_dict(),
        "required_configs": attr.string_list(),
        "tools": attr.label_keyed_string_dict(
            allow_files = True,
            default = {
                Label("//cyber/mainboard"): "bin/mainboard",
                Label("//cyber/tools/cyber_launch:cyber_launch.py"): "bin/cyber_launch.py",
                Label("//cyber/tools/cyber_recorder"): "bin/cyber_recorder",
                Label("//cyber/tools/cyber_monitor"): "bin/cyber_monitor",
            },
        ),
        "python_runtime": attr.label_list(default = [Label("//runtime:python_runtime")]),
        "_materializer": attr.label(default = Label("//runtime:materialize_runtime"), executable = True, cfg = "exec"),
        "_empty_file": attr.label(default = Label("//runtime:empty"), allow_single_file = True),
    },
)

def _run_impl(ctx):
    root = ctx.attr.runtime[RuntimeLayoutInfo].root
    launcher = ctx.actions.declare_file(ctx.label.name + ".bash")
    key = _runfile_key(root, ctx.workspace_name)
    ctx.actions.write(launcher, """#!/usr/bin/env bash
set -euo pipefail
runfiles="${RUNFILES_DIR:-$0.runfiles}"
root="$runfiles/%s"
exec python3 "$root/share/core/runtime_entry.py" "$@"
""" % key, is_executable = True)
    return [DefaultInfo(executable = launcher, runfiles = ctx.runfiles(files = [root]))]

runtime_run = rule(
    implementation = _run_impl,
    executable = True,
    attrs = {"runtime": attr.label(mandatory = True, providers = [RuntimeLayoutInfo])},
)

def _package_impl(ctx):
    info = ctx.attr.runtime[RuntimeLayoutInfo]
    root = info.root
    executable_tree = ctx.actions.declare_directory(ctx.label.name + ".executables")
    data_tree = ctx.actions.declare_directory(ctx.label.name + ".data")
    ctx.actions.run(
        executable = ctx.executable._materializer,
        arguments = ["--package", "--modes", info.modes.path, root.path, executable_tree.path, data_tree.path],
        inputs = [root, info.modes],
        tools = [ctx.attr._materializer[DefaultInfo].files_to_run],
        outputs = [executable_tree, data_tree],
        mnemonic = "RuntimePackageModes",
    )

    # Distinct equivalent root keys let rules_pkg merge disjoint mode trees.
    return [
        PackageFilegroupInfo(
            pkg_files = [
                (PackageFilesInfo(dest_src_map = {".": executable_tree}, attributes = {"mode": "0755"}), ctx.label),
                (PackageFilesInfo(dest_src_map = {"": data_tree}, attributes = {"mode": "0644"}), ctx.label),
            ],
            pkg_dirs = [],
            pkg_symlinks = [],
        ),
        DefaultInfo(files = depset([executable_tree, data_tree])),
    ]

runtime_package = rule(
    implementation = _package_impl,
    attrs = {
        "runtime": attr.label(mandatory = True, providers = [RuntimeLayoutInfo]),
        "_materializer": attr.label(default = Label("//runtime:materialize_runtime"), executable = True, cfg = "exec"),
    },
)

def _required_impl(ctx):
    fail(ctx.attr.message)

required_runtime_input = rule(
    implementation = _required_impl,
    attrs = {"message": attr.string(default = "Supply the required runtime configuration input.")},
)
