"""Publish an explicitly accepted draft; normal AI version tags stay draft-only."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.request import urlopen

from ai_release import PACKAGES, TAG_PATTERN, command, digest, release_state, require_draft, verify_packages

ASSETS = {*PACKAGES.values(), "SHA256SUMS.txt", "BUILD_INFO.json"}
REQUIRED_CHECKS = {"windows_package", "linux_package", "px4_sitl", "edge_cases", "settings_upgrade"}


def validate_acceptance(acceptance):
    if not re.fullmatch(TAG_PATTERN, acceptance["tag"]):
        raise ValueError("Invalid accepted release tag")
    if not re.fullmatch(r"[0-9a-f]{40}", acceptance["commit"]):
        raise ValueError("A full accepted source commit is required")
    if set(acceptance["sha256"]) != ASSETS:
        raise ValueError("Acceptance must identify exactly the four release assets")
    if any(not re.fullmatch(r"[0-9a-f]{64}", value) for value in acceptance["sha256"].values()):
        raise ValueError("Invalid accepted checksum")
    for check in REQUIRED_CHECKS:
        if acceptance["checks"][check]["result"] != "passed" or not acceptance["checks"][check]["details"].strip():
            raise ValueError(f"Required acceptance is incomplete: {check}")
    for check in ("api_path", "chatgpt_path"):
        result = acceptance["checks"][check]
        if result["result"] not in ("passed", "limited") or not result["details"].strip():
            raise ValueError(f"AI service outcome must be recorded: {check}")


def main():
    acceptance = json.loads(Path("tools/ai_release/ACCEPTANCE.json").read_text(encoding="utf-8"))
    validate_acceptance(acceptance)
    repo, tag, sha = os.environ["GITHUB_REPOSITORY"], acceptance["tag"], acceptance["commit"]
    if os.environ["GITHUB_REF_NAME"] != f"codex/ai-publish-{tag}":
        raise ValueError("Publishing requires the explicit version-specific acceptance branch")
    if command("git", "rev-parse", f"refs/tags/{tag}^{{commit}}") != sha:
        raise ValueError("Accepted source does not match the release tag")
    subprocess.run(["git", "merge-base", "--is-ancestor", sha, "HEAD"], check=True)
    changed = set(command("git", "diff", "--name-only", sha, "HEAD").splitlines())
    if changed != {"tools/ai_release/ACCEPTANCE.json"}:
        raise ValueError("The publication commit may only add the acceptance record to the tagged source")
    run = json.loads(command("gh", "api", f"repos/{repo}/actions/runs/{acceptance['run_id']}"))
    if (run["head_sha"] != sha or run["conclusion"] != "success" or
            run["path"] != ".github/workflows/ai-release.yml" or run["event"] != "push"):
        raise ValueError("Accepted source must have a successful AI release workflow run")
    release = release_state(repo, tag)
    if release is None:
        raise ValueError("The tested release draft does not exist")
    require_draft(release)
    if {asset["name"] for asset in release["assets"]} != ASSETS:
        raise ValueError("Unexpected or missing draft assets")

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        subprocess.run(["gh", "release", "download", tag, "--repo", repo, "--dir", temporary], check=True)
        for name, expected in acceptance["sha256"].items():
            if digest(directory / name) != expected:
                raise ValueError(f"Draft differs from the accepted download: {name}")
        info = json.loads((directory / "BUILD_INFO.json").read_text(encoding="utf-8"))
        expected = {"commit": sha, "tag": tag, "repository": repo, "run_id": str(acceptance["run_id"])}
        if any(info[key] != value for key, value in expected.items()):
            raise ValueError("Draft build information differs from acceptance")
        for build in info["builds"]:
            platform = build["platform"]
            if platform not in PACKAGES:
                raise ValueError("Unexpected build platform")
            (directory / f"build-info-{platform}.json").write_text(json.dumps(build), encoding="utf-8")
        verify_packages(directory, expected)
        sums = "".join(f"{digest(directory / name)}  {name}\n" for name in [*PACKAGES.values(), "BUILD_INFO.json"])
        if (directory / "SHA256SUMS.txt").read_text(encoding="utf-8") != sums:
            raise ValueError("Checksum list does not match the accepted packages")

        notes = release["body"]
        placeholder = "Manual package/SITL acceptance and live AI-service checks must be recorded here before publication. A successful build alone is not evidence that those checks passed."
        if placeholder not in notes:
            raise ValueError("Draft acceptance placeholder is missing")
        outcomes = "\n".join(f"- **{check} — {result['result']}**: {result['details']}" for check, result in acceptance["checks"].items())
        notes = notes.replace(placeholder, outcomes)
        require_draft(release_state(repo, tag))
        payload = {"draft": False, "prerelease": True, "make_latest": "false", "body": notes}
        subprocess.run(["gh", "api", "--method", "PATCH", f"repos/{repo}/releases/{release['id']}", "--input", "-"],
                       input=json.dumps(payload), text=True, check=True, stdout=subprocess.DEVNULL)

        public_hashes = {}
        for name, expected_hash in acceptance["sha256"].items():
            destination = directory / ("public-" + name)
            # No token: verify the exact public download URLs readers will use.
            with urlopen(f"https://github.com/{repo}/releases/download/{tag}/{name}", timeout=180) as response:
                with destination.open("wb") as output:
                    while chunk := response.read(1024 * 1024):
                        output.write(chunk)
            public_hashes[name] = digest(destination)
            if public_hashes[name] != expected_hash:
                raise ValueError(f"Public download does not match the accepted artifact: {name}")
        Path("PUBLIC_DOWNLOAD_VERIFICATION.json").write_text(json.dumps({
            **expected, "release_url": f"https://github.com/{repo}/releases/tag/{tag}",
            "publication_run_id": os.environ["GITHUB_RUN_ID"], "public_sha256": public_hashes,
            "result": "All four public downloads match the accepted draft assets"
        }, indent=2) + "\n", encoding="utf-8")
        print("Published accepted experimental prerelease; all public download hashes match.")


if __name__ == "__main__":
    main()
