# RFC: PX4 AI Flight Diagnostics in QGroundControl Analyze View

## Status

- Stage: phase-one implementation
- Product status: experimental and unofficial
- Firmware scope: PX4 only
- Base: QGroundControl v5.0.8 experimental branch

This document is written so that it can be adapted into an upstream QGroundControl issue or RFC. It describes the implemented boundary; it is not a claim that the feature has been accepted upstream.

## Motivation

The prototype placed an AI assistant beside MAVLink Console. A QGroundControl maintainer pointed out that MAVLink Console is PX4-only while QGroundControl features should normally have a broader product boundary, and recommended a new Analyze page.

Phase one follows the structural part of that feedback: diagnostics are now a dedicated Analyze tool and MAVLink Console is again an independent shell UI. Firmware extensibility is represented by a provider interface, while the only registered implementation is intentionally PX4.

## Goals

- Give PX4 users one place to ask evidence-grounded diagnostic questions.
- Keep AI service coordination separate from firmware evidence collection.
- Preserve the prototype's read-only tools, command/message whitelists, approval gates, and call limits.
- Make the transmitted data boundary visible, especially for pasted Console output.
- Reject unsupported firmware before a prompt, tool, or network request is built.
- Preserve existing experimental settings through the rename.
- Keep a clear extension point for a future, separately designed firmware provider.

## Non-goals

- ArduPilot support in phase one
- Flight control, mode changes, arming, takeoff, landing, or motion
- Parameter writes or resets
- Calibration, actuator tests, reboot, mission/geofence/rally-point modification
- Arbitrary shell or MAVLink execution
- Background Console collection or a shared shell service
- Autonomous fault repair or a replacement for pilot judgement

## User experience

The Analyze menu contains **AI Flight Diagnostics (PX4)** next to the existing MAVLink Console entry.

The page contains:

- Experimental, Unofficial, and PX4-only labels
- Active vehicle, firmware, armed/flying, link, and data-availability summary
- Conversation transcript and question input
- Cancel, Clear, and Settings actions
- An approval card for gated MAVLink data requests
- A collapsed, user-paste-only PX4 Console evidence field
- A visible warning that pasted data is sent to the selected service

Question availability is deterministic:

| State | Ask | Local tools | Network request |
| --- | --- | --- | --- |
| Connected PX4 | Allowed when service is configured | According to policy | Allowed |
| Connected non-PX4 | Disabled | Disabled | Disabled |
| No vehicle, no attachment | Disabled | Disabled | Disabled |
| No vehicle, attachment present | Allowed | Disabled | Allowed |

Only the final 12,000 characters of an attachment are sent. The field is cleared with the conversation and when the active vehicle changes.

## Architecture

![Analyze diagnostics data flow](assets/ai-assistant/ai-flight-diagnostics-data-flow.svg)

### AIDiagnosticController

This is the QML-facing coordinator. It owns conversation state, request lifecycle, cancellation, approval state, service selection, network interaction, and the locally enforced tool executor. It does not define PX4 diagnostic semantics.

Every request is bound to the active Vehicle at request start. Active-vehicle change, vehicle destruction, or communication loss cancels the request and clears pending approval. Tool execution uses the bound Vehicle rather than looking up a new active Vehicle.

Cancellation also invalidates the asynchronous request generation. ChatGPT notifications are accepted only when both their thread and turn identifiers match the current request, preventing a late event from a canceled request from entering a newer conversation.

### AIDiagnosticContextBuilder

This class builds the versioned JSON supplied to either service path. It serializes core state, sensor summary, health/arming checks, link state, FactGroups, batteries, provider evidence, and the explicit Console attachment.

It does not collect a shell stream and does not infer firmware-specific meaning.

### AIDiagnosticProvider

This is the firmware-extension boundary. A provider identifies supported Vehicles, contributes structured diagnostic evidence, and supplies firmware-specific prompt rules.

The boundary exists to avoid baking future firmware support into the generic controller. Adding another implementation is explicitly outside phase one.

### PX4DiagnosticProvider

The phase-one provider owns:

- PX4 Vehicle eligibility
- External-vision evidence construction
- Sensor target normalization and Console query mapping
- Read-only Console command and parameter-name policy
- Whitelisted MAVLink message IDs
- Armed/flying/link checks for low-privilege MAVLink data requests
- Natural-language automatic diagnostic routing
- PX4-specific evidence interpretation rules

![PX4 provider safety architecture](assets/ai-assistant/ai-flight-diagnostics-provider-safety.svg)

## Versioned diagnostic context

The top-level schema is version 1:

| Field | Purpose |
| --- | --- |
| schemaVersion | Contract version, currently 1 |
| source | QGroundControl AI Flight Diagnostics |
| timestampUtc | Snapshot creation time |
| activeVehicle | Whether a live Vehicle contributed data |
| firmware | Availability, support flag, type, and MAVLink type ID |
| core | Vehicle identity, flight state, mode, readiness, position, and counters |
| sysStatusSensorInfo | QGroundControl's sensor summary |
| healthAndArmingCheckReport | Current Events-based health/arming evidence |
| linkStatus | Communication loss, packet counters, radio status, and signing |
| factGroups | Serialized QGroundControl FactGroups |
| batteries | Serialized battery FactGroups |
| px4DiagnosticEvidence | Provider evidence, including external-vision layers |
| consoleAttachment | Availability, source, truncation state, and explicit text |

Unavailable evidence is represented explicitly instead of being omitted or inferred as a fault.

## AI service paths

### OpenAI-compatible Chat Completions

The request contains the versioned context and a structured local tool registry. A model may propose a tool call, but QGroundControl parses and validates it locally. The model never supplies an executable free-form command.

### ChatGPT through Codex App Server

The request is created with read-only sandbox policy, no approval policy, and no network access for local execution. QGroundControl sends context and receives answer text. This path does not expose the local diagnostic tool registry.

The service and task identifiers, prompt text, and HTTP User-Agent identify PX4 AI Flight Diagnostics rather than MAVLink Console AI.

## Tool policy

| Tool class | Examples | Approval | Vehicle-state gate |
| --- | --- | --- | --- |
| In-memory read | status, FactGroup, health, link, loaded parameter | No | Bound PX4 Vehicle |
| Restricted PX4 Console read | sensors status, listener of a fixed topic, param show | No | PX4, disarmed, not flying, link available |
| One-shot MAVLink data request | REQUEST_MESSAGE for a whitelisted ID | Yes | PX4, disarmed, not flying, link available |
| Temporary telemetry request | SET_MESSAGE_INTERVAL, at most 5 Hz and 30 seconds | Yes | PX4, disarmed, not flying, link available |

Limits are enforced per question: one model tool round, at most three PX4 Console commands, and at most one low-privilege MAVLink request. Temporary intervals are restored to the default after their TTL.

The registry contains no parameter-write, command-long passthrough, mode, mission, calibration, actuator, or flight-control tool.

## Privacy boundary

- Console evidence is supplied only through an explicit paste field.
- No background collector or shared Console controller exists.
- The UI warns the user before transmission and reports truncation.
- API keys remain in the existing local QSettings group.
- Both service paths receive only the data assembled for the current request and limited conversation history.
- The remote service's own retention and account terms remain applicable.

## Settings migration

The C++ and QML surface is renamed from AIConsoleSettings to AIAssistantSettings.

- New QML property: <code>aiAssistantSettings</code>
- Temporary compatibility alias: <code>aiConsoleSettings</code>
- QSettings group retained: <code>AIConsole</code>

Keeping the storage group preserves endpoint URL, model, API key, ChatGPT model choice, and deprecated OAuth fields from the prototype.

## Failure handling

| Failure | Behavior |
| --- | --- |
| Unsupported firmware | Reject before context/tool/network request |
| No Vehicle and no attachment | Reject locally |
| Armed, flying, or communication lost | Skip/reject Vehicle-side diagnostic requests |
| Vehicle switch, destruction, or disconnect | Cancel request and clear approval |
| Tool outside registry or malformed arguments | Return a rejected tool result |
| Call limit exceeded | Reject additional calls |
| Timeout or network error | Stop busy state and show an actionable error |
| Missing evidence | Mark unavailable/unknown; do not invent a value |

## Test matrix

Automated coverage includes:

- Analyze page, AI settings page, and pure MAVLink Console QML loading
- Legacy settings-group and compatibility-alias checks
- Versioned no-Vehicle attachment context
- PX4/non-PX4 provider eligibility
- Parameter, Console-command, and MAVLink-message whitelists
- Automatic PX4 external-vision routing
- External-vision input, configuration, fusion, invalid, unknown, and unavailable semantics
- Codex App Server protocol parsing and client behavior

Manual acceptance should cover PX4 SITL state, parameter, external-vision, approval, cancellation, vehicle switch, armed/flying, communication-loss, and attachment-only flows on Linux and Windows.

## Follow-up direction

After phase-one validation:

1. Collect maintainer feedback on Analyze navigation, naming, and provider boundaries.
2. Expand PX4 evidence adapters only where data semantics and safety checks are testable.
3. Add redaction assistance and a request-data preview.
4. Evaluate local-model support independently of the diagnostic policy.
5. Consider another firmware provider only through a separate proposal, implementation owner, and test matrix.
