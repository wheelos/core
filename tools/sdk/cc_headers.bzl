"""Collect explicit API roots and their required template support headers."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _workspace_path(header):
    path = header.short_path
    if path.startswith("../"):
        return "/".join(path.split("/")[2:])
    return path

def _cc_sdk_headers_impl(ctx):
    public_headers = []
    support_headers = []
    for target in ctx.attr.targets:
        public_headers.extend(target[CcInfo].compilation_context.direct_public_headers)
        support_headers.extend([
            header
            for header in target[CcInfo].compilation_context.headers.to_list()
            if header.owner and
               header.owner.workspace_name == ctx.label.workspace_name and
               _workspace_path(header).startswith("cyber/")
        ])

    headers = depset(support_headers)
    public = depset(public_headers)
    manifest = ctx.actions.declare_file(ctx.label.name + ".all.txt")
    public_manifest = ctx.actions.declare_file(ctx.label.name + ".public.txt")
    ctx.actions.write(manifest, "\n".join(sorted([_workspace_path(f) for f in headers.to_list()])) + "\n")
    ctx.actions.write(public_manifest, "\n".join(sorted([_workspace_path(f) for f in public.to_list()])) + "\n")
    return [
        DefaultInfo(files = headers),
        OutputGroupInfo(
            public_headers = public,
            manifest = depset([manifest]),
            public_manifest = depset([public_manifest]),
        ),
    ]

cc_sdk_headers = rule(
    implementation = _cc_sdk_headers_impl,
    attrs = {
        "targets": attr.label_list(
            mandatory = True,
            providers = [CcInfo],
        ),
    },
)
