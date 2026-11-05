"""Custom package installer rule preserving execroot-relative short_path runfiles."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load(
    "@rules_pkg//pkg:providers.bzl",
    "PackageDirsInfo",
    "PackageFilegroupInfo",
    "PackageFilesInfo",
    "PackageSymlinkInfo",
)
load(
    "@rules_pkg//pkg/private:pkg_files.bzl",
    "ENTRY_IS_FILE",
    "ENTRY_IS_LINK",
    "create_mapping_context_from_ctx",
    "process_src",
)
load("@rules_python//python:defs.bzl", "py_binary")

def _encode_manifest_entry(ctx, dest, df):
    entry_type = df.entry_type if hasattr(df, "entry_type") else ENTRY_IS_FILE
    repository = None
    if df.src:
        # In Bazel, File.short_path is always relative to ctx.workspace_name,
        # including externally owned _solib_k8 files that lack a "../" prefix.
        repository = ctx.workspace_name
        src = df.src.short_path
    elif hasattr(df, "link_to"):
        src = df.link_to
        entry_type = ENTRY_IS_LINK
    else:
        src = None

    origin_str = str(df.origin)
    if not origin_str.startswith("@"):
        origin_str = "@" + origin_str

    return json.encode({
        "type": entry_type,
        "src": src,
        "dest": dest.strip("/"),
        "mode": df.mode or "",
        "user": df.user or None,
        "group": df.group or None,
        "uid": df.uid,
        "gid": df.gid,
        "origin": origin_str,
        "repository": repository,
    })

def _write_short_path_manifest(ctx, manifest_file, content_map):
    ctx.actions.write(
        manifest_file,
        "[\n" + ",\n".join(
            [
                _encode_manifest_entry(ctx, dst, content_map[dst])
                for dst in sorted(content_map.keys())
            ],
        ) + "\n]\n",
    )

def _whl_pkg_install_script_impl(ctx):
    script_file = ctx.actions.declare_file(ctx.attr.name + ".py")

    mapping_context = create_mapping_context_from_ctx(ctx, label = ctx.label, default_mode = "0644")
    for src in ctx.attr.srcs:
        process_src(
            mapping_context,
            src = src,
            origin = src.label,
        )

    manifest_file = ctx.actions.declare_file(ctx.attr.name + "-install-manifest.json")

    destdir = ctx.attr.destdir
    if ctx.attr.destdir_flag:
        if BuildSettingInfo in ctx.attr.destdir_flag:
            destdir = ctx.attr.destdir_flag[BuildSettingInfo].value

    _write_short_path_manifest(ctx, manifest_file, mapping_context.content_map)

    label_str = str(ctx.label)[:-len("_install_script")]

    ctx.actions.expand_template(
        template = ctx.file._script_template,
        output = script_file,
        substitutions = {
            "{MANIFEST_INCLUSION}": manifest_file.short_path,
            "{WORKSPACE_NAME}": ctx.workspace_name,
            "{TARGET_LABEL}": label_str,
            "{DEFAULT_DESTDIR}": destdir,
        },
        is_executable = True,
    )

    my_runfiles = ctx.runfiles(
        files = [manifest_file],
        transitive_files = depset(
            direct = mapping_context.file_deps_direct,
            transitive = mapping_context.file_deps_transitive,
        ),
    )

    return [
        DefaultInfo(
            files = depset([script_file]),
            runfiles = my_runfiles,
            executable = script_file,
        ),
    ]

_whl_pkg_install_script = rule(
    implementation = _whl_pkg_install_script_impl,
    attrs = {
        "srcs": attr.label_list(
            mandatory = True,
            providers = [
                [PackageFilegroupInfo],
                [PackageFilesInfo],
                [PackageDirsInfo],
                [PackageSymlinkInfo],
            ],
        ),
        "destdir": attr.string(),
        "destdir_flag": attr.label(),
        "_script_template": attr.label(
            allow_single_file = True,
            default = Label("@rules_pkg//pkg/private:install.py.tpl"),
        ),
    },
    executable = True,
)

def pkg_install(name, srcs, destdir = None, destdir_flag = None, **kwargs):
    """Create a bazel run-nable installer script that resolves short_path runfiles."""
    if destdir and destdir_flag:
        fail("You may only set one of destdir or destdir_flag")

    _whl_pkg_install_script(
        name = name + "_install_script",
        srcs = srcs,
        destdir = destdir,
        destdir_flag = destdir_flag,
        **kwargs
    )

    py_binary(
        name = name,
        srcs = [":" + name + "_install_script"],
        main = name + "_install_script.py",
        deps = [
            Label("@rules_pkg//pkg/private:manifest"),
            Label("@rules_python//python/runfiles"),
        ],
        srcs_version = "PY3",
        python_version = "PY3",
        **kwargs
    )
