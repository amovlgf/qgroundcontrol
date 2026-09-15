<!-- Draft: post only after ai.2 is public and both public downloads are verified. -->

The Windows and Linux packages for the dedicated Analyze-view assistant are
available in **v5.0.8.1-ai.2**:

[Download QGroundControl AI Flight Diagnostics v5.0.8.1-ai.2](https://github.com/amovlgf/qgroundcontrol/releases/tag/v5.0.8.1-ai.2)

Open **Analyze Tools → AI Flight Diagnostics**. AI diagnostics currently support
PX4. MAVLink Console remains a separate tool; Console text is supplied to the
assistant only when explicitly attached.

The earlier `v5.0.8.1-ai.1` download contained the older Console-integrated
prototype. It did not include the dedicated Analyze page described in this
update. Please use the explicit ai.2 link above for Windows x64 or Linux
x86_64. The release includes SHA256 checksums, source/build information, test
results and known limitations.

This remains an experimental, unofficial build. The assistant provides
diagnostic analysis and bounded evidence requests; it cannot perform flight
control, write parameters or run arbitrary commands. Vehicle-side diagnostic
requests are blocked while armed, flying or disconnected. AI output still
requires independent verification.
