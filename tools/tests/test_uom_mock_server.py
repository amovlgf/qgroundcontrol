"""The local simulator must remain explicit and reject expired/wrong sessions."""

from uom_mock_server import MockService


def request(generation="2", sequence="0", digest="a" * 64):
    return {
        "protocol": "uom-test-v1",
        "environment": "mock",
        "generation": generation,
        "sequence": sequence,
        "identity": {"digest": digest},
        "telemetry": {},
    }


def test_authentication_and_report():
    service = MockService()
    status, response = service.response("/uom-test-v1/auth", request(), "")
    assert status == 200
    assert response["environment"] == "mock"
    assert response["accepted"]
    token = "Bearer " + response["token"]
    assert service.response("/uom-test-v1/report", request(sequence="1"), token)[0] == 200
    assert service.response("/uom-test-v1/report", request(generation="3"), token)[0] == 401
    assert service.response("/uom-test-v1/report", request(digest="b" * 64), token)[0] == 401


def test_expired_tokens_and_fault_recovery():
    service = MockService()
    _, first = service.response("/uom-test-v1/auth", request(), "")
    _, second = service.response("/uom-test-v1/auth", request(), "")
    assert first["token"] != second["token"]
    assert service.response("/uom-test-v1/report", request(), "Bearer " + first["token"])[0] == 401
    service.response("/control/fault", {"fault": "unauthorized", "phase": "report", "count": 1}, "")
    assert service.response("/uom-test-v1/report", request(), "Bearer " + second["token"])[0] == 401
    assert service.response("/uom-test-v1/report", request(), "Bearer " + second["token"])[0] == 200


def test_formal_protocol_is_not_supported():
    service = MockService()
    payload = request()
    payload["environment"] = "official"
    assert service.response("/uom-test-v1/auth", payload, "")[0] == 400
    assert service.response("/control/fault", {"fault": "unknown"}, "")[0] == 400
    assert service.response("/official/uom", request(), "")[0] == 404
