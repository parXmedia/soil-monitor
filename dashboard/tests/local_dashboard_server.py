"""Local-only emulator for browser-testing the ESP32 dashboard and controls."""

from argparse import ArgumentParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "include" / "web_dashboard.h").read_text(encoding="utf-8")
MATCH = re.search(r'const char DASHBOARD_HTML\[\].*?R"HTML\((.*?)\)HTML";', HEADER, re.DOTALL)
if MATCH is None:
    raise RuntimeError("Could not extract local dashboard HTML")
PAGE = MATCH.group(1).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    mode = "live"
    update_ready = False

    def send_bytes(self, content, content_type, status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def send_json(self, value, status=200):
        self.send_bytes(json.dumps(value).encode(), "application/json", status)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            self.send_bytes(PAGE, "text/html; charset=utf-8")
            return
        if path == "/api/history":
            self.send_json({"moisture": [39, 41, 43, 46, 48, 47]})
            return
        if path == "/api/data":
            seconds = {"live": 2, "low_power": 300}[self.mode]
            live = self.mode == "live"
            self.send_json({
                "linked": True, "fresh": True, "valid": True,
                "moisture": 47, "condition": "MOISTURE GOOD", "raw": 2184,
                "millivolts": 1760, "rssi": -72, "signal": 56, "age_ms": 800,
                "sequence": 12345, "boot_id": 77, "next_sample_seconds": seconds,
                "requested_sample_seconds": seconds, "sampling_mode": self.mode,
                "battery_millivolts": 4050, "current_milliamps": 96 if live else 0,
                "power_milliwatts": 389 if live else 0, "power_available": live,
                "power_measured": live, "sensor_firmware_build": 196609,
                "sensor_protocol": 4, "received": 1200, "missed": 3,
                "reliability": 99.8, "samples": 60, "average": 46.8,
                "minimum": 39, "maximum": 52, "delta": 1.2, "trend": "Stable",
                "calibration_dry": 2513, "calibration_wet": 1300,
                "calibrated": True, "cloud_pending": 0, "cloud_dropped": 0,
                "cloud_buffer": "flash", "cloud_online": True,
                "sensor_update_ready": self.update_ready,
                "sensor_update_uploading": False,
                "sensor_update_build": 196610 if self.update_ready else 0,
                "sensor_update_size": 854129 if self.update_ready else 0,
                "sensor_update_error": "", "gateway_firmware": "3.0.0",
                "ip": "127.0.0.1",
            })
            return
        self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path == "/api/mode":
            requested = json.loads(body or b"{}").get("mode")
            if requested not in {"live", "low_power"}:
                self.send_json({"error": "invalid_mode"}, 400)
                return
            type(self).mode = requested
        elif self.path == "/api/sensor-update":
            type(self).update_ready = True
            self.send_json({"ok": True, "build": 196610, "size": len(body)}, 201)
            return
        self.send_json({"ok": True})

    def do_DELETE(self):
        if self.path == "/api/sensor-update":
            type(self).update_ready = False
            self.send_json({"ok": True})
            return
        self.send_error(404)

    def log_message(self, fmt, *args):
        pass


def main():
    parser = ArgumentParser()
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Local dashboard preview: http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
