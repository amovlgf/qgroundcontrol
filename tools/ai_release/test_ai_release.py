"""Protect against publishing stale, partial or modified release artifacts."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from ai_release import PACKAGES, digest, require_draft, verify_packages, verify_test_report
from publish_ai_release import ASSETS, REQUIRED_CHECKS, validate_acceptance


class ReleaseValidationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.expected = {"commit": "a" * 40, "tag": "v5.0.8.1-ai.2", "repository": "owner/repo", "run_id": "123"}
        self.metadata = {}
        for platform, name in PACKAGES.items():
            path = self.directory / name
            path.write_bytes(f"example {platform} package".encode())
            info = dict(self.expected, platform=platform, package=name, qt_version="6.8.3",
                        application_version=self.expected["tag"], sha256=digest(path), size=path.stat().st_size)
            self.metadata[platform] = info
            self.write_info(platform, info)

    def write_info(self, platform, info):
        (self.directory / f"build-info-{platform}.json").write_text(json.dumps(info))

    def test_accepts_matching_packages(self):
        self.assertEqual(len(verify_packages(self.directory, self.expected)), 2)

    def test_rejects_wrong_commit_tag_run_or_repository(self):
        for key in self.expected:
            with self.subTest(key=key):
                info = copy.deepcopy(self.metadata["windows"])
                info[key] = "wrong"
                self.write_info("windows", info)
                with self.assertRaises(ValueError):
                    verify_packages(self.directory, self.expected)
        self.write_info("windows", self.metadata["windows"])

    def test_rejects_missing_platform(self):
        (self.directory / PACKAGES["linux"]).unlink()
        with self.assertRaises(ValueError):
            verify_packages(self.directory, self.expected)

    def test_rejects_changed_package(self):
        (self.directory / PACKAGES["windows"]).write_bytes(b"another build")
        with self.assertRaises(ValueError):
            verify_packages(self.directory, self.expected)

    def test_rejects_wrong_version(self):
        info = self.metadata["linux"]
        info["application_version"] = "v5.0.8.1-ai.1"
        self.write_info("linux", info)
        with self.assertRaises(ValueError):
            verify_packages(self.directory, self.expected)

    def test_never_overwrites_published_release(self):
        require_draft(None)
        require_draft({"draft": True})
        with self.assertRaises(ValueError):
            require_draft({"draft": False})

    def test_test_report_cannot_be_empty_partial_or_skipped(self):
        path = self.directory / "ai-tests.xml"
        good = '<testcase name="AIDiagnosticControllerTest" status="run"/><testcase name="CodexAppServerClientTest" status="run"/>'
        path.write_text('<testsuite>' + good + '</testsuite>')
        self.assertEqual(len(verify_test_report(path)), 2)
        for body in ('', '<testcase name="AIDiagnosticControllerTest"/>',
                     good.replace('status="run"/>', 'status="notrun"/>', 1),
                     good.replace('status="run"/>', 'status="run"><failure/></testcase>', 1),
                     good.replace('status="run"/>', 'status="run"><skipped/></testcase>', 1)):
            with self.subTest(body=body):
                path.write_text('<testsuite>' + body + '</testsuite>')
                with self.assertRaises(ValueError):
                    verify_test_report(path)

    def test_acceptance_requires_complete_package_and_sitl_checks(self):
        acceptance = {
            "tag": self.expected["tag"], "commit": self.expected["commit"],
            "sha256": {name: "b" * 64 for name in ASSETS},
            "checks": {name: {"result": "passed", "details": "Recorded evidence"}
                       for name in REQUIRED_CHECKS | {"api_path", "chatgpt_path"}},
        }
        validate_acceptance(acceptance)
        for name in REQUIRED_CHECKS:
            with self.subTest(check=name):
                incomplete = copy.deepcopy(acceptance)
                incomplete["checks"][name]["result"] = "limited"
                with self.assertRaises(ValueError):
                    validate_acceptance(incomplete)
        acceptance["checks"]["chatgpt_path"] = {"result": "limited", "details": "Live account unavailable; protocol tests passed"}
        validate_acceptance(acceptance)
        acceptance["sha256"].pop("BUILD_INFO.json")
        with self.assertRaises(ValueError):
            validate_acceptance(acceptance)


if __name__ == "__main__":
    unittest.main()
