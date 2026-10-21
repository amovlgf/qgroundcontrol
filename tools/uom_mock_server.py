#!/usr/bin/env python3
"""Local uom-test-v1 simulator. This is not a Chinese UOM API or regulatory submission."""

from __future__ import annotations

import argparse
import contextlib
import json
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


class MockService:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.tokens: dict[str, tuple[str, str]] = {}
        self.fault: dict[str, Any] = {}

    def response(self, path: str, payload: dict[str, Any], authorization: str) -> tuple[int, Any]:
        if path == "/control/fault":
            if payload.get("fault") not in {
                "none",
                "unauthorized",
                "reject",
                "malformed",
                "mismatch",
                "delay",
                "unavailable",
            }:
                return 400, {"error": "unknown fault"}
            with self.lock:
                self.fault = dict(payload)
                self.fault["count"] = max(0, min(int(payload.get("count", 1)), 1000))
            return 200, {"environment": "mock", "configured": True}
        if path not in {"/uom-test-v1/auth", "/uom-test-v1/report"}:
            return 404, {"error": "unknown endpoint"}
        if payload.get("protocol") != "uom-test-v1" or payload.get("environment") != "mock":
            return 400, {"error": "simulation protocol required"}
        identity = payload.get("identity", {})
        digest = identity.get("digest", "")
        generation = payload.get("generation", "")
        if not isinstance(digest, str) or len(digest) != 64 or not isinstance(generation, str):
            return 400, {"error": "invalid identity or session"}
        with self.lock:
            phase = "auth" if path.endswith("auth") else "report"
            fault = (
                dict(self.fault)
                if self.fault.get("count", 0) and self.fault.get("phase", "any") in {"any", phase}
                else {}
            )
            if fault:
                self.fault["count"] -= 1
        mode = fault.get("fault")
        if mode == "delay":
            time.sleep(min(max(int(fault.get("delay_ms", 4000)), 0), 10000) / 1000)
        if mode == "unauthorized":
            return 401, {"error": "injected authorization expiry"}
        if mode == "unavailable":
            return 503, {"error": "injected outage"}
        if mode == "malformed":
            return 200, b"{not-json"
        result = {
            "protocol": "uom-test-v1",
            "environment": "mock",
            "generation": generation,
            "sequence": payload.get("sequence"),
            "identity_digest": digest,
            "accepted": mode != "reject",
        }
        if mode == "mismatch":
            result["identity_digest"] = "0" * 64
        with self.lock:
            if phase == "auth":
                token = secrets.token_hex(24)
                # Bound memory and keep at most one token per aircraft session.
                self.tokens = {
                    key: value
                    for key, value in self.tokens.items()
                    if value != (generation, digest)
                }
                if len(self.tokens) >= 256:
                    self.tokens.pop(next(iter(self.tokens)))
                self.tokens[token] = (generation, digest)
                result["token"] = token
            elif self.tokens.get(
                authorization[7:] if authorization.startswith("Bearer ") else ""
            ) != (generation, digest):
                return 401, {"error": "unknown session token"}
        return 200, result


def run() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    service = MockService()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self) -> None:
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if size <= 0 or size > 16384:
                    raise ValueError("invalid body size")
                payload = json.loads(self.rfile.read(size))
                if not isinstance(payload, dict):
                    raise ValueError("object required")
                status, result = service.response(
                    self.path, payload, self.headers.get("Authorization", "")
                )
            except (ValueError, TypeError, AttributeError):
                status, result = 400, {"error": "invalid request"}
            body = result if isinstance(result, bytes) else json.dumps(result).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            with contextlib.suppress(BrokenPipeError, ConnectionResetError):
                self.wfile.write(body)

        def log_message(self, fmt: str, *values: Any) -> None:
            # Do not log identities, authorization headers or telemetry.
            print("MOCK UOM:", fmt % values, flush=True)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"SIMULATION ONLY: http://127.0.0.1:{server.server_port}; not official UOM", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    run()
