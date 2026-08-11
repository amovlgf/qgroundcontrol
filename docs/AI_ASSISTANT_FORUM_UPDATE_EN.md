# Update: Moving the Safety-Bounded PX4 AI Assistant to QGroundControl Analyze View

This is a follow-up to [Building a Safety-Bounded AI Flight Diagnostic Assistant in QGroundControl](https://discuss.px4.io/t/building-a-safety-bounded-ai-flight-diagnostic-assistant-in-qgroundcontrol/49228).

## Why the direction changed

A QGroundControl maintainer commented that the concept looked interesting, but that it did not belong inside MAVLink Console. The key point was architectural: MAVLink Console is PX4-only, while a QGroundControl feature should have an appropriate product boundary. The suggested direction was a new Analyze page.

This update implements that direction without claiming broader firmware support that does not yet exist:

- The assistant is now a dedicated **AI Flight Diagnostics (PX4)** Analyze page.
- MAVLink Console is restored to a standalone Console tool.
- Firmware-specific logic is behind a provider boundary.
- Phase one deliberately registers only the PX4 provider.

This remains an experimental, unofficial branch—not an upstream QGroundControl feature.

## New Analyze experience

![AI Flight Diagnostics Analyze page](assets/ai-assistant/ai-flight-diagnostics-analyze-en.png)

The new page shows the connected firmware, armed/flight and link state, and the diagnostic data that may be available. It contains the conversation, question input, cancel/clear/settings actions, and a tool-approval card.

Console evidence is no longer taken from the MAVLink Console UI. A collapsed attachment area accepts text only when the user deliberately pastes it. The page explains that the text will be sent to the selected service, limits it to the final 12,000 characters, and clears it with the session or Vehicle change.

With no Vehicle, attachment-only analysis is possible but every local tool is disabled. With a connected non-PX4 Vehicle, diagnosis is disabled before any AI request is sent.

## Refactored architecture

![Updated QGroundControl AI diagnostic data flow](assets/ai-assistant/ai-flight-diagnostics-data-flow.svg)

The previous controller has been split into:

- **AIDiagnosticController** — QML-facing conversation, request, cancellation, approval, service, and tool-execution coordination.
- **AIDiagnosticContextBuilder** — versioned QGroundControl context containing core state, sensor summary, health/arming checks, link state, FactGroups, batteries, PX4 evidence, and explicit Console attachment.
- **AIDiagnosticProvider** — firmware diagnostic extension boundary.
- **PX4DiagnosticProvider** — PX4 eligibility, external-vision evidence, parameter and Console policy, automatic question routing, MAVLink data-message whitelist, and PX4 prompt semantics.

The context begins with schemaVersion 1 so future changes can be reviewed as an explicit data contract.

## Safety boundary retained

![PX4 provider and safety checks](assets/ai-assistant/ai-flight-diagnostics-provider-safety.svg)

The assistant still cannot arm, disarm, move, take off, land, change modes, write/reset parameters, calibrate, reboot, run actuator tests, edit missions, or execute arbitrary shell/MAVLink commands.

The OpenAI-compatible path exposes only a fixed local read-only registry. Restricted PX4 Console requests use a command whitelist and per-question limit. REQUEST_MESSAGE and temporary SET_MESSAGE_INTERVAL remain limited to whitelisted telemetry messages and require explicit user approval.

Vehicle-side Console and MAVLink requests are rejected when the PX4 Vehicle is armed, flying, or communication is lost. A request is bound to the Vehicle active when it starts; switching, disconnecting, or destroying that Vehicle cancels the request and clears approval.

The ChatGPT/Codex path remains context-and-answer only. It is started read-only and cannot invoke QGroundControl's local tools.

## Settings compatibility

![AI Assistant settings](assets/ai-assistant/ai-assistant-settings.png)

The settings interface is now named AIAssistantSettings and QML uses aiAssistantSettings. A temporary aiConsoleSettings alias remains for compatibility. The underlying QSettings group is still AIConsole, preserving endpoint, model, API key, ChatGPT model selection, and legacy fields from the prototype.

## Verification

The updated branch includes automated coverage for:

- The new Analyze page, AI settings page, and standalone MAVLink Console page
- Versioned attachment-only context
- Legacy settings storage and compatibility alias
- PX4 versus non-PX4 provider eligibility
- Parameter, Console command, and MAVLink message whitelists
- Automatic external-vision diagnostic routing and evidence semantics
- Codex App Server client protocol behavior

The final branch is also checked with a Debug build and the bilingual VitePress documentation build. PX4 SITL remains the manual acceptance environment for state, parameter, external-vision, approval, cancellation, vehicle-switch, armed/flying, communication-loss, and attachment-only scenarios.

## Scope and next steps

ArduPilot is intentionally not part of phase one. The next useful discussion is whether the Analyze-page UX, provider boundary, context contract, and safety rules are suitable—not whether a nominal firmware label can be added without equivalent diagnostics and tests.

Potential follow-ups are:

1. A request-data preview and redaction assistance.
2. More PX4 evidence adapters with explicit semantics and tests.
3. Broader Linux/Windows SITL coverage.
4. Evaluation of local models as a service option.
5. A separate proposal for any additional firmware provider.

Feedback on the new Analyze placement and the boundary between QGroundControl data, PX4-specific evidence, and AI service behavior is welcome.
