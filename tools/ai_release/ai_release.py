#!/usr/bin/env python3
"""Collect same-run AI builds and create draft-only experimental releases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

PACKAGES = {
    "windows": "QGroundControl-installer.exe",
    "linux": "QGroundControl-x86_64.AppImage",
}
FEATURE_COMMIT = "823585e0e02aca598f7f8c35bbb9a6161aeab766"
TAG_PATTERN = r"v\d+\.\d+\.\d+(?:\.\d+)?-ai\.\d+"


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def identity():
    sha = command("git", "rev-parse", "HEAD")
    if sha != os.environ["GITHUB_SHA"]:
        raise ValueError("Checkout does not match the workflow source commit")
    tag = os.environ["GITHUB_REF_NAME"] if os.environ["GITHUB_REF_TYPE"] == "tag" else None
    if tag:
        if not re.fullmatch(TAG_PATTERN, tag):
            raise ValueError("Invalid AI release tag")
        if command("git", "rev-parse", f"refs/tags/{tag}^{{commit}}") != sha:
            raise ValueError("Tag does not match the checked out commit")
        if command("git", "describe", "--tags", "--exact-match") != tag:
            raise ValueError("Application version does not match the release tag")
    return {"commit": sha, "tag": tag, "repository": os.environ["GITHUB_REPOSITORY"],
            "run_id": os.environ["GITHUB_RUN_ID"]}


def release_state(repo, tag):
    result = subprocess.run(["gh", "api", f"repos/{repo}/releases/tags/{tag}"],
                            text=True, capture_output=True, check=False)
    if result.returncode == 0:
        return json.loads(result.stdout)
    if "HTTP 404" in result.stderr:
        return None
    raise RuntimeError(result.stderr)


def require_draft(release):
    if release is not None and not release["draft"]:
        raise ValueError("Refusing to change an already published release")


def validate():
    info = identity()
    subprocess.run(["git", "merge-base", "--is-ancestor", FEATURE_COMMIT, info["commit"]], check=True)
    if info["tag"]:
        require_draft(release_state(info["repository"], info["tag"]))
    print(json.dumps(info, indent=2))


def record(directory, platform):
    info = identity()
    name = PACKAGES[platform]
    package = directory / name
    if not package.is_file() or package.stat().st_size == 0:
        raise ValueError(f"Missing or empty package: {name}")
    info.update(platform=platform, package=name, sha256=digest(package), size=package.stat().st_size,
                qt_version=os.environ["QT_VERSION"], run_attempt=os.environ["GITHUB_RUN_ATTEMPT"],
                application_version=command("git", "describe", "--always", "--tags"))
    (directory / f"build-info-{platform}.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")


def verify_packages(directory, expected):
    builds = []
    for platform, name in PACKAGES.items():
        info = json.loads((directory / f"build-info-{platform}.json").read_text(encoding="utf-8"))
        for key, value in expected.items():
            if info[key] != value:
                raise ValueError(f"{platform}: {key} does not match this workflow")
        if info["platform"] != platform or info["package"] != name or info["qt_version"] != "6.8.3":
            raise ValueError(f"{platform}: unexpected package configuration")
        package = directory / name
        if not package.is_file() or package.stat().st_size == 0:
            raise ValueError(f"Missing or empty package: {name}")
        if digest(package) != info["sha256"] or package.stat().st_size != info["size"]:
            raise ValueError(f"{platform}: package checksum or size mismatch")
        if expected["tag"] and info["application_version"] != expected["tag"]:
            raise ValueError(f"{platform}: embedded version source does not match tag")
        builds.append(info)
    return builds


def collect(directory):
    expected = identity()
    builds = verify_packages(directory, expected)
    tests = verify_test_report(directory / "ai-tests.xml")
    run_url = f"https://github.com/{expected['repository']}/actions/runs/{expected['run_id']}"
    info = dict(expected, run_url=run_url, builds=builds, passed_test_suites=tests)
    (directory / "BUILD_INFO.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    names = [*PACKAGES.values(), "BUILD_INFO.json"]
    (directory / "SHA256SUMS.txt").write_text("".join(f"{digest(directory / name)}  {name}\n" for name in names), encoding="utf-8")
    template = Path("tools/ai_release/RELEASE_NOTES.md").read_text(encoding="utf-8")
    for key, value in {"VERSION": expected["tag"] or "unreleased validation build", "COMMIT": expected["commit"],
                       "REPOSITORY": expected["repository"], "RUN_URL": run_url}.items():
        template = template.replace("{{" + key + "}}", value)
    (directory / "RELEASE_NOTES.md").write_text(template, encoding="utf-8")


def verify_test_report(path):
    root = ET.parse(path).getroot()
    cases = root.findall(".//testcase")
    expected = {"AIDiagnosticControllerTest", "CodexAppServerClientTest"}
    if len(cases) != 2 or {case.get("name") for case in cases} != expected:
        raise ValueError("Both AI test suites must appear in the executed test report")
    for case in cases:
        if any(case.find(marker) is not None for marker in ("failure", "error", "skipped")):
            raise ValueError("Failed or skipped AI test suite")
        if case.get("status", "run") not in ("run", "passed"):
            raise ValueError("AI test suite was not executed")
    return sorted(expected)


def draft(directory):
    info = identity()
    if not info["tag"]:
        raise ValueError("Only a version tag can create a draft release")
    verify_packages(directory, info)
    repo, tag = info["repository"], info["tag"]
    existing = release_state(repo, tag)
    require_draft(existing)
    payload = {"tag_name": tag, "target_commitish": info["commit"], "draft": True,
               "prerelease": True, "make_latest": "false",
               "name": f"QGroundControl AI Flight Diagnostics {tag} — Experimental / Unofficial",
               "body": (directory / "RELEASE_NOTES.md").read_text(encoding="utf-8")}
    path = f"repos/{repo}/releases" + (f"/{existing['id']}" if existing else "")
    subprocess.run(["gh", "api", "--method", "PATCH" if existing else "POST", path, "--input", "-"],
                   input=json.dumps(payload), text=True, check=True)
    # Check again before uploading, in case someone published the draft meanwhile.
    require_draft(release_state(repo, tag))
    assets = [str(directory / name) for name in [*PACKAGES.values(), "SHA256SUMS.txt", "BUILD_INFO.json"]]
    subprocess.run(["gh", "release", "upload", tag, "--repo", repo, "--clobber", *assets], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("validate", "record", "collect", "draft"))
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--platform", choices=PACKAGES)
    args = parser.parse_args()
    if args.action == "validate":
        validate()
    elif args.directory is None:
        parser.error("--directory is required")
    elif args.action == "record":
        if not args.platform:
            parser.error("--platform is required")
        record(args.directory, args.platform)
    elif args.action == "collect":
        collect(args.directory)
    else:
        draft(args.directory)


if __name__ == "__main__":
    main()
