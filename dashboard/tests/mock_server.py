"""Local-only dashboard server with realistic, synthetic API responses."""

from argparse import ArgumentParser
from datetime import datetime, timedelta, timezone
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
from socketserver import TCPServer
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]


class LocalThreadingHTTPServer(ThreadingHTTPServer):
    """HTTP server that avoids unnecessary reverse-DNS lookup on localhost."""

    def server_bind(self):
        TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = host
        self.server_port = port


class MockDashboardHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def send_json(self, payload, status=200):
        content = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def do_GET(self):
        path = urlparse(self.path).path
        now = datetime.now(timezone.utc)

        if path == "/config.js":
            port = self.server.server_address[1]
            source = f'''window.SOIL_MONITOR_CONFIG = Object.freeze({{
              supabaseUrl: "http://127.0.0.1:{port}",
              publishableKey: "sb_publishable_12345678901234567890",
              apiBaseUrl: "http://127.0.0.1:{port}",
              currentPath: "/v1/current",
              historyPath: "/v1/history",
              pollIntervalMs: 5000,
              requestTimeoutMs: 3000,
              staleAfterMs: 30000,
              thresholds: Object.freeze({{ dryBelow: 25, healthyBelow: 80 }})
            }});'''.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(source)))
            self.end_headers()
            self.wfile.write(source)
            return

        if path == "/vendor/supabase-2.105.0.js":
            source = b'''window.supabase = { createClient() {
              const session = { access_token: "header.payload.signature", user: { email: "owner@example.com" } };
              return { auth: {
                getSession: async () => ({ data: { session }, error: null }),
                onAuthStateChange: () => ({ data: { subscription: { unsubscribe() {} } } }),
                signInWithPasskey: async () => ({ data: { session }, error: null }),
                signInWithOtp: async () => ({ error: null }),
                registerPasskey: async () => ({ data: { id: "demo-passkey" }, error: null }),
                passkey: { list: async () => ({ data: [{ friendly_name: "Mock passkey" }], error: null }) },
                signOut: async () => ({ error: null })
              }};
            }};'''
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(source)))
            self.end_headers()
            self.wfile.write(source)
            return

        if path == "/v1/current":
            if self.headers.get("Authorization") != "Bearer header.payload.signature":
                self.send_json({"error": "unauthorized"}, status=401)
                return
            self.send_json({
                "deviceId": "garden-01",
                "timestamp": now.isoformat(),
                "moisture": 47.3,
                "rawAdc": 2184,
                "millivolts": 1760,
                "rssi": -72,
                "signal": 56,
                "batteryVoltage": 4.05,
                "batteryPercent": 82,
                "sequence": 12345,
                "currentMilliamps": 96,
                "powerMilliwatts": 389,
                "powerMeasured": True,
                "samplingMode": "live",
                "sensorFirmwareBuild": 196609,
                "sensorFirmware": "3.0.1",
                "gatewayFirmware": "3.0.0",
            })
            return

        if path == "/v1/history":
            if self.headers.get("Authorization") != "Bearer header.payload.signature":
                self.send_json({"error": "unauthorized"}, status=401)
                return
            readings = []
            for index in range(97):
                timestamp = now - timedelta(minutes=(96 - index) * 15)
                moisture = 46 + math.sin(index / 8) * 4 + index / 96
                readings.append({
                    "deviceId": "garden-01",
                    "timestamp": timestamp.isoformat(),
                    "moisture": round(moisture, 1),
                    "rssi": -72,
                    "batteryPercent": 82,
                })
            self.send_json({"readings": readings})
            return

        super().do_GET()


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=8080, type=int)
    args = parser.parse_args()
    server = LocalThreadingHTTPServer(("127.0.0.1", args.port), MockDashboardHandler)
    print(f"Dashboard preview: http://127.0.0.1:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
