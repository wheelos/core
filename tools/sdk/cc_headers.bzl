"""Collect public headers owned by the current workspace from a C++ dependency graph."""

SdkHeadersInfo = provider(fields = ["headers"])

def _sdk_headers_aspect_impl(target, ctx):
    transitive_headers = []
    for dep in ctx.rule.attr.deps if hasattr(ctx.rule.attr, "deps") else []:
        if SdkHeadersInfo in dep:
            transitive_headers.append(dep[SdkHeadersInfo].headers)

    direct_headers = []
    if CcInfo in target:
        for header in target[CcInfo].compilation_context.direct_public_headers:
            if header.short_path.startswith("cyber/"):
                direct_headers.append(header)

    return [SdkHeadersInfo(
        headers = depset(direct_headers, transitive = transitive_headers),
    )]

sdk_headers_aspect = aspect(
    implementation = _sdk_headers_aspect_impl,
    attr_aspects = ["deps"],
)

def _cc_sdk_headers_impl(ctx):
    return [DefaultInfo(files = ctx.attr.target[SdkHeadersInfo].headers)]

cc_sdk_headers = rule(
    implementation = _cc_sdk_headers_impl,
    attrs = {
        "target": attr.label(
            mandatory = True,
            aspects = [sdk_headers_aspect],
            providers = [CcInfo],
        ),
    },
)
