# AI experimental releases

Create an annotated `v*-ai.*` tag on the verified AI source commit. The
`AI Experimental Release` workflow builds Windows x64 and Linux x86_64 with
Qt 6.8.3, tests the Linux Debug build, installs and starts the Windows package,
and starts the Linux AppImage. Both reusable workflows check out the same
commit and recursive submodules. Other platform workflows exclude AI tags.

The packages retain the `QGroundControl` application/settings identity used by
ai.1, including the existing `AIConsole` settings group. Experimental status is
shown in the version tag and release notes.

Only a successful tag run creates a **draft prerelease**. Branches named
`codex/ai-release-*` run the same validation without creating a Release. The
collector requires both packages from the current run and verifies source,
version, platform, Qt version, size and SHA256. A rerun refuses to modify any
already published release. Old tags and assets are retained.

Before publishing, download the draft's packages and verify `SHA256SUMS.txt`.
Record installed-package and PX4 SITL acceptance, including page navigation,
explicit attachments, cancellation, switching/disconnecting vehicles, armed
and flying restrictions, unsupported firmware and legacy settings. Record the
API and ChatGPT/Codex paths separately, with actual limitations. The automated
Windows check skips USB driver installation. Linux packages target Ubuntu
22.04 x86_64 or a compatible runtime.

To publish through Actions, start a branch `codex/ai-publish-<tag>` at the exact
tagged commit. Add only `tools/ai_release/ACCEPTANCE.json`, containing `tag`, the
full `commit`, successful AI build `run_id`, a `sha256` mapping for all four
downloaded assets, and `checks`. Each check has `result` and `details`. Required
checks are `windows_package`, `linux_package`, `px4_sitl`, `edge_cases` and
`settings_upgrade`, all `passed`. Record `api_path` and `chatgpt_path` separately
as `passed` or `limited` with a concrete explanation. Never mark unperformed
checks as passed. Push this acceptance-only commit **after acceptance**.

The publication workflow checks the tag, successful build run and all accepted
hashes against the existing draft, adds the actual acceptance outcomes to the
notes, publishes the prerelease, and downloads all public assets without
authentication to compare their hashes again. It refuses to change an already
public release. `PUBLIC_DOWNLOAD_VERIFICATION.json` records the results. The forum
update in `FORUM_UPDATE_EN.md` is a draft and must only be posted after that
public verification. Always link to the explicit version page.

Local validation of the release collector:

```sh
python3 -m unittest discover -s tools/ai_release -p 'test_*.py' -v
```
