"""Analysis regressions for module ownership and composition."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load(":runtime.bzl", "RuntimeModuleInfo", "required_runtime_input", "runtime_layout", "runtime_module", "runtime_resources")

def _module_impl(ctx):
    env = analysistest.begin(ctx)
    info = analysistest.target_under_test(env)[RuntimeModuleInfo]
    asserts.equals(env, "example", info.name)
    destinations = [destination for _, destination in info.entries]
    asserts.true(env, "conf/example/example.conf" in destinations)
    asserts.true(env, "assets/models/example/model.txt" in destinations)
    asserts.true(env, "models/example/model.txt" in destinations)
    asserts.true(env, "conf/example/generated/" in destinations)
    asserts.true(env, len(info.libraries.to_list()) > 0)
    return analysistest.end(env)

def _failure_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected)
    return analysistest.end(env)

def _override_impl(ctx):
    env = analysistest.begin(ctx)
    manifest = analysistest.target_under_test(env)[OutputGroupInfo].manifest.to_list()[0]
    asserts.true(env, manifest.basename.endswith(".manifest.json"))
    return analysistest.end(env)

_module_test = analysistest.make(_module_impl)
_override_test = analysistest.make(_override_impl)
_failure_test = analysistest.make(_failure_impl, expect_failure = True, attrs = {"expected": attr.string()})

def _directory_impl(ctx):
    directory = ctx.actions.declare_directory(ctx.label.name)
    ctx.actions.run_shell(
        outputs = [directory],
        arguments = [directory.path],
        command = 'mkdir -p "$1"; printf "declared tree\\n" > "$1/generated.conf"',
    )
    return [DefaultInfo(files = depset([directory]))]

_directory_fixture = rule(implementation = _directory_impl)

def runtime_analysis_tests(name):
    """Declare ownership/composition tests.

    Args:
        name: Name of the suite grouping the analysis tests.
    """
    runtime_resources(
        name = "example_resources",
        files = ["tests/models/model.txt"],
        module_name = "example",
    )
    _directory_fixture(name = "directory_fixture", testonly = True)
    runtime_module(
        name = "example_module",
        testonly = True,
        components = [":fixture_dependency", ":libfixture_component.so"],
        executables = [":fixture_diagnose"],
        files = {
            "tests/example.conf": "conf/example/example.conf",
            ":directory_fixture": "conf/example/generated/",
        },
        resources = [":example_resources"],
        module_name = "example",
    )
    _module_test(name = "module_analysis_test", target_under_test = ":example_module")
    for case, destination, expected in [
        ("reserved", "conf/cyber.pb.conf", "shared Cyber configuration is owned by runtime_layout"),
        ("traversal", "conf/example/../bad", "unsafe runtime destination"),
        ("foreign", "conf/other/bad", "cannot own"),
    ]:
        runtime_module(
            name = case + "_module",
            testonly = True,
            tags = ["manual"],
            module_name = "example",
            files = {"tests/example.conf": destination},
        )
        _failure_test(
            name = case + "_analysis_test",
            target_under_test = ":" + case + "_module",
            expected = expected,
        )
    runtime_module(
        name = "duplicate_module",
        testonly = True,
        module_name = "example",
    )
    runtime_layout(
        name = "duplicate_layout",
        testonly = True,
        tags = ["manual"],
        modules = [":example_module", ":duplicate_module"],
        tools = {},
        python_runtime = [],
        cyber_conf = "tests/example.conf",
    )
    _failure_test(
        name = "duplicate_analysis_test",
        target_under_test = ":duplicate_layout",
        expected = "duplicate runtime module name",
    )
    runtime_layout(
        name = "both_defaults",
        testonly = True,
        tags = ["manual"],
        default_dag = "dag/example/a.dag",
        default_launch = "launch/example/a.launch",
        tools = {},
        python_runtime = [],
        cyber_conf = "tests/example.conf",
    )
    _failure_test(
        name = "defaults_analysis_test",
        target_under_test = ":both_defaults",
        expected = "default_dag and default_launch are mutually exclusive",
    )
    required_runtime_input(
        name = "missing_input",
        testonly = True,
        tags = ["manual"],
        message = "reviewed input required",
    )
    _failure_test(
        name = "required_input_analysis_test",
        target_under_test = ":missing_input",
        expected = "reviewed input required",
    )
    runtime_layout(
        name = "product_override_layout",
        testonly = True,
        modules = [":example_module"],
        files = {
            "tests/models/model.txt": "conf/example/example.conf",
        },
        tools = {},
        python_runtime = [],
        cyber_conf = "tests/example.conf",
    )
    _override_test(
        name = "product_override_analysis_test",
        target_under_test = ":product_override_layout",
    )
    native.test_suite(
        name = name,
        tests = [
            ":module_analysis_test",
            ":reserved_analysis_test",
            ":traversal_analysis_test",
            ":foreign_analysis_test",
            ":duplicate_analysis_test",
            ":defaults_analysis_test",
            ":required_input_analysis_test",
            ":product_override_analysis_test",
        ],
    )
