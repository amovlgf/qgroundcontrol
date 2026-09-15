# QGroundControl AI Flight Diagnostics {{VERSION}}

**Experimental / Unofficial / AI diagnostics currently PX4 only.**

This build contains the dedicated **Analyze Tools → AI Flight Diagnostics** page described in the [Analyze-view forum update](https://discuss.px4.io/t/update-moving-the-safety-bounded-px4-ai-assistant-to-qgroundcontrol-analyze-view/49488). MAVLink Console is a separate tool again. The older `v5.0.8.1-ai.1` package contains the Console-integrated prototype.

## Downloads and provenance

- Windows x64: `QGroundControl-installer.exe`
- Linux x86_64: `QGroundControl-x86_64.AppImage` (built on Ubuntu 22.04)
- Verify downloads using `SHA256SUMS.txt`; `BUILD_INFO.json` records both packages, source, Qt version, hashes and workflow run.
- Source commit: [`{{COMMIT}}`](https://github.com/{{REPOSITORY}}/commit/{{COMMIT}})
- Build and automated validation: [GitHub Actions]({{RUN_URL}})
- QGroundControl base: v5.0.8; Qt: 6.8.3.

## Changes from ai.1

- A dedicated conversation and vehicle-status page under Analyze Tools.
- PX4 Console text is an explicit, optional attachment; the final 12,000 characters are sent, and attachments are cleared when the session or active vehicle changes.
- Firmware-provider separation, versioned diagnostic context, and stronger isolation of pending requests when vehicles change or disconnect.
- Existing AI endpoint, model and account settings remain in the `AIConsole` settings group.

## AI service paths

- OpenAI-compatible API: configure your own endpoint, model and API key. Structured diagnostic requests are validated locally; allowed MAVLink data requests require user approval.
- ChatGPT through a separately installed local Codex App Server: sign in on your own machine. This path receives supplied context and returns analysis; QGC local tools are not exposed.
- No API keys, login state or personal settings are included in the packages.

The assistant cannot arm, disarm, move the vehicle, change modes, write parameters or execute arbitrary commands. Vehicle-side diagnostic requests are blocked when armed, flying or disconnected. Read-only analysis does not replace pilot judgment.

## Validation and limitations

The draft is generated only after both platform builds, Linux AI controller/protocol tests, Linux AppImage boot and Windows installed-package boot and normal main-window startup succeed. Windows USB driver installation is not part of the automated package check.

Manual package/SITL acceptance and live AI-service checks must be recorded here before publication. A successful build alone is not evidence that those checks passed.

AI replies may time out or contain errors. Missing diagnostic evidence remains unknown. ArduPilot AI diagnostics, macOS, Android and ARM packages are outside this release.
