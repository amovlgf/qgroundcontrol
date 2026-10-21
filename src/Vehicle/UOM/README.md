# RID identity and UOM reporting

`VehicleUomManager` belongs to one `Vehicle`. Active-vehicle selection only changes
which status the toolbar displays; it does not stop another vehicle's session.
PX4 firmware advertises the extension by exposing the `COM_RID_UOM` Fact. Its value
controls the product's preflight requirement. Other firmware never receives these
project queries. QGC reads identity after parameter discovery, polls every five
seconds, and invalidates it after ten seconds without a valid response. Identity
reads have a 22-second deadline to cover the complete CAN snapshot. Failed queries
use bounded retries and a ten-second recovery interval.

The identity digest and MAVLink fields are defined in
[`protocol/amov_rid_uom/README.md`](../../../../protocol/amov_rid_uom/README.md).
QGC recomputes BLAKE2b-256, validates field lengths and binds the PX4 boot, RID
boot, revision and digest to a random nonzero connection session. A connection
loss or invalidation notification retires the session; old query responses and
old HTTP callbacks cannot restore it. Empty registration remains unknown. The
broadcast suffix is never used as the complete registration identifier.

Activation is performed through the RID Wi-Fi page with a signed package. QGC
has no activation upload or device-writing path. It displays the effective
activation state, effective and pending revisions, and the last activation result
separately. Staged and already-active packages are displayed as successful results.

## Providers and readiness

The production provider is deliberately `UnconfiguredUomProvider`. No official
endpoint, credentials, authority validation, reporting schema, frequency, retry
contract, or retention policy has been provided; it never reports success.
Implementing a real provider requires those approved inputs and contract tests
against the official test environment. A simulator response is not official
acceptance or compliance certification.

Only an explicit `-DQGC_UOM_BENCH_TEST=ON` build can select `MockUomProvider`, and
only when `QGC_UOM_MOCK_URL` is a loopback HTTP URL. The UI and wire environment
always identify this as MOCK. Production PX4 rejects MOCK; testing readiness
requires the separately built PX4 bench image. A normal QGC build ignores the
mock URL entirely.

The mock session authenticates asynchronously, reports at 1 Hz, accepts only
correlated replies, and expires an acknowledgement after five seconds. Aircraft
telemetry must be younger than three seconds; LIVE control-station observations
must be younger than one second and have an ellipsoid altitude. Repeated GPS UTC
or GLOBAL_POSITION_INT sample timestamps do not refresh their age. FIXED control
station configuration represents a deliberately confirmed WGS84 ellipsoid point.

There is at most one HTTP request in flight and one latest telemetry snapshot.
There is no unbounded offline backlog or replay of stale positions. A failed or
three-second overdue request cancels the operation, invalidates authentication
and readiness, and retries after 1, 2, then 4 seconds. Retrying requests rebinds
to the current identity. A 1 Hz MAVLink status heartbeat continues during failures.

UOM prevents new arming/takeoff/mission-start readiness only while disarmed and
`COM_RID_UOM=1`. An in-flight reporting loss changes the status display, records
a warning, and attempts recovery. It does not issue flight commands. PX4 remains
the authority for arming and checks RID health independently.

## Local simulator

```sh
python3 tools/uom_mock_server.py --port 8765
QGC_UOM_MOCK_URL=http://127.0.0.1:8765 bash tools/amov_m4/run_qgc.sh
```

Run QGC with the matching Qt runtime/container as documented by the build setup.
The server only binds 127.0.0.1. It never forwards data to an external platform.
It logs HTTP paths and response codes, not tokens, identities or positions.

Inject faults into the next requests; `phase` is `auth`, `report`, or `any`:

```sh
curl -X POST http://127.0.0.1:8765/control/fault \
  -H 'Content-Type: application/json' \
  -d '{"fault":"unauthorized","phase":"report","count":1}'
curl -X POST http://127.0.0.1:8765/control/fault \
  -H 'Content-Type: application/json' \
  -d '{"fault":"delay","delay_ms":4000,"count":2}'
```

Other faults: `reject`, `malformed`, `mismatch`, `unavailable`, `none`.
Stop/restart the server to exercise loss of connectivity and token recovery.

The private test contract uses POST `/uom-test-v1/auth` and `/uom-test-v1/report`.
Requests contain `protocol="uom-test-v1"`, `environment="mock"`, decimal-string
`generation` and `sequence`, `identity`, and a telemetry snapshot with explicit
units. Auth uses sequence `"0"`; reporting uses the returned bearer token.
Successful replies echo protocol, environment, generation, sequence, identity
hex digest, and `accepted=true`; auth additionally returns `token`. Wrong identity,
session, sequence, environment, invalid JSON, non-200 HTTP, rejection, and timeout
all fail closed. Tokens are memory-only. Redirects are not followed.

## Automated tests

With the configured Qt/compiler environment:

```sh
cmake -S test/Vehicle/UOM -B build-uom-tests -G Ninja -DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"
cmake --build build-uom-tests
ctest --test-dir build-uom-tests --output-on-failure
ctest --test-dir build-amov-m4 -R 'VehicleUomManagerTest|RemoteIDManagerTest|VehicleReadiness' --output-on-failure
```

The standalone tests cover independent digest vectors, per-aircraft isolation,
activation/freshness gates, never claiming official success, stale callbacks,
request deadlines, real loopback HTTP authentication/reporting, malformed and
mismatched replies, cancellation, outage and recovery. The application tests
send real serialized MAVLink through MockLink and the normal parser, verifying
MAVLink 2 negotiation, identity and status binding, wrong digest rejection,
query timeouts and connection loss. Readiness tests retain armed flight actions.
Remote ID tests check truthful altitude datum and control-station freshness.

Physical CAN timing, GNSS accuracy, production signing keys, platform approval,
radio broadcast conformance and aircraft flight behavior still require the
separate hardware acceptance procedure.
