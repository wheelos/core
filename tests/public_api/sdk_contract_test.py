import os
import re
import sys
import tarfile
import unittest
from pathlib import Path


class SdkContractTest(unittest.TestCase):
    def test_documented_extensions_and_schemas_are_public_sdk_roots(self):
        public = set(Path(sys.argv[2]).read_text().splitlines())
        expected = {
            "cyber/tools/cyber_recorder/record/core/recorder.h",
            *(
                f"cyber/proto/{schema}.pb.h"
                for schema in (
                    "clock",
                    "component_conf",
                    "dag_conf",
                    "parameter",
                    "qos_profile",
                    "role_attributes",
                    "record",
                    "run_mode_conf",
                )
            ),
        }
        self.assertLessEqual(expected, public)
        self.assertNotIn(
            "cyber/tools/cyber_recorder/record/core/recorder_config.h", public
        )

    def test_module_dependency_direction(self):
        self.assertEqual(Path(sys.argv[6]).read_text().strip(), "")

    def test_application_target_does_not_reexport_runtime_implementation(self):
        headers = set(Path(sys.argv[5]).read_text().splitlines())
        self.assertIn("cyber/cyber.h", headers)
        self.assertNotIn("cyber/io/poll_handler.h", headers)
        self.assertNotIn("cyber/sysmo/sysmo.h", headers)
        self.assertNotIn("cyber/internal/python_lifecycle.h", headers)
        self.assertNotIn("cyber/class_loader/class_loader_manager.h", headers)

    def test_packaged_sdk_matches_header_inventory(self):
        headers = set(Path(sys.argv[1]).read_text().splitlines())
        public = set(Path(sys.argv[2]).read_text().splitlines())
        with tarfile.open(sys.argv[4]) as archive:
            members = {
                member.name.removeprefix("./"): member
                for member in archive.getmembers()
            }
            self.assertLessEqual(
                {"usr/local/include/" + header for header in headers}, set(members)
            )
            for suffix, expected in (("public", public), ("all", headers)):
                manifest = members[
                    "usr/local/share/wheelos_core/"
                    f"wheelos_cyber_headers.{suffix}.txt"
                ]
                with archive.extractfile(manifest) as stream:
                    self.assertEqual(set(stream.read().decode().splitlines()), expected)

    def test_api_visibility(self):
        visible = set(Path(sys.argv[3]).read_text().splitlines())
        self.assertEqual(
            visible,
            {
                "//cyber:cyber",
                "//cyber/node:node",
                "//cyber/service:service",
                "//cyber/service:client",
                "//cyber/gpu:gpu_reader",
                "//cyber/gpu:gpu_writer",
            },
        )

    def test_template_support_headers_are_complete_but_not_public_api(self):
        headers = set(Path(sys.argv[1]).read_text().splitlines())
        public = set(Path(sys.argv[2]).read_text().splitlines())
        self.assertLessEqual(public, headers)
        self.assertIn("cyber/gpu/gpu_reader.h", public)
        self.assertIn("cyber/gpu/gpu_writer.h", public)
        self.assertNotIn("cyber/node/internal/intra_reader.h", public)
        self.assertNotIn("cyber/gpu/internal/gpu_control_endpoint.h", public)
        self.assertNotIn("cyber/io/poll_handler.h", headers)
        self.assertNotIn("cyber/sysmo/sysmo.h", headers)
        self.assertNotIn("cyber/internal/python_lifecycle.h", headers)
        root = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        missing = []
        for header in sorted(headers):
            text = (root / header).read_text()
            for included in re.findall(
                r'^\s*#\s*include\s*["<](cyber/[^">]+)[">]', text, re.MULTILINE
            ):
                if included not in headers:
                    missing.append(f"{header} -> {included}")
        self.assertEqual(missing, [], "\n".join(missing))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
