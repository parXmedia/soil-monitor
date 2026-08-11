from html.parser import HTMLParser
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = ROOT.parent


class DashboardParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.local_assets = []
        self.external_assets = []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if "id" in attributes:
            self.ids.add(attributes["id"])
        asset = attributes.get("src") or (attributes.get("href") if tag == "link" else None)
        if asset:
            if asset.startswith(("http://", "https://", "//")):
                self.external_assets.append(asset)
            else:
                self.local_assets.append(asset)


class DashboardStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = (ROOT / "index.html").read_text(encoding="utf-8")
        cls.script = "\n".join(
            (ROOT / name).read_text(encoding="utf-8")
            for name in ("app.js", "auth.js", "data.js")
        )
        cls.config = (ROOT / "config.js").read_text(encoding="utf-8")
        cls.local_dashboard = (PROJECT_ROOT / "include" / "web_dashboard.h").read_text(encoding="utf-8")
        cls.protocol = (PROJECT_ROOT / "include" / "soil_protocol.h").read_text(encoding="utf-8")
        cls.parser = DashboardParser()
        cls.parser.feed(cls.html)

    def test_all_page_assets_are_local_and_present(self):
        self.assertEqual([], self.parser.external_assets)
        for asset in self.parser.local_assets:
            clean_path = asset.split("?", 1)[0].lstrip("./")
            self.assertTrue((ROOT / clean_path).is_file(), asset)

    def test_expected_live_regions_exist(self):
        expected = {
            "connection-pill", "notice", "moisture-value", "history-chart",
            "last-reading", "signal-value", "battery-value", "access-gate",
            "passkey-sign-in-button", "setup-email", "manage-passkeys-button",
            "account-panel", "add-passkey-button", "sign-out-button"
        }
        self.assertTrue(expected.issubset(self.parser.ids))

    def test_javascript_dom_references_exist(self):
        referenced = set(re.findall(r'document\.getElementById\("([^"]+)"\)', self.script))
        array_match = re.search(r"const ids = \[(.*?)\];", self.script, re.DOTALL)
        self.assertIsNotNone(array_match)
        referenced.update(re.findall(r'"([a-z][a-z0-9-]+)"', array_match.group(1)))
        self.assertEqual(set(), referenced - self.parser.ids)

    def test_config_has_no_obvious_secret_fields(self):
        forbidden = {"password", "secret", "privatekey", "service_role", "wifipassword", "token", "apikey"}
        property_names = {
            name.lower()
            for name in re.findall(r"^\s*([A-Za-z_$][\w$]*)\s*:", self.config, re.MULTILINE)
        }
        self.assertEqual(set(), forbidden & property_names)

    def test_security_policy_restricts_active_content(self):
        self.assertIn("object-src 'none'", self.html)
        self.assertIn("base-uri 'none'", self.html)
        self.assertIn("form-action 'none'", self.html)
        self.assertIn("upgrade-insecure-requests", self.html)

    def test_passkey_session_replaces_shared_read_key(self):
        self.assertIn("signInWithPasskey", self.script)
        self.assertIn("registerPasskey", self.script)
        self.assertIn("experimental: { passkey: true }", self.script)
        self.assertIn("Authorization: `Bearer ${session.access_token}`", self.script)
        self.assertNotIn("soil-monitor-read-token", self.script)
        self.assertNotIn("this.readToken", self.script)

    def test_null_optional_telemetry_stays_unavailable(self):
        self.assertRegex(
            self.script,
            r'function finiteNumber\(value, fallback = null\) \{\s*'
            r'if \(value === null \|\| value === undefined \|\| value === ""\) '
            r'return fallback;',
        )

    def test_freshness_is_bounded_and_last_update_is_visible(self):
        self.assertIn("pollIntervalMs: 2000", self.config)
        self.assertIn("const MIN_POLL_INTERVAL_MS = 2000", self.script)
        self.assertIn("staleAfterMs: 360000", self.config)
        self.assertIn('freshness.fresh ? "Up to date" : "Stale data"', self.script)
        self.assertIn("formatLastUpdate(reading.timestamp)", self.script)
        self.assertNotIn('freshness.fresh ? "Live"', self.script)

    def test_local_sampling_control_is_explicit_and_protocol_backed(self):
        self.assertIn('id="modeToggle"', self.local_dashboard)
        self.assertIn('role="switch"', self.local_dashboard)
        self.assertIn("event.target.checked?'live':'low_power'", self.local_dashboard)
        self.assertIn("control('/api/mode'", self.local_dashboard)
        self.assertIn("'X-Soil-Control':'local-dashboard'", self.local_dashboard)
        self.assertIn("requestedSampleSeconds", self.protocol)
        self.assertIn("SOIL_PACKET_VERSION = 4", self.protocol)

    def test_power_and_wireless_update_controls_exist(self):
        self.assertIn('id="power"', self.local_dashboard)
        self.assertIn('id="updateForm"', self.local_dashboard)
        self.assertIn("/api/sensor-update", self.local_dashboard)
        self.assertIn('id="power-value"', self.html)
        self.assertIn('id="mode-value"', self.html)
        self.assertIn("awakeWindowMs", self.protocol)
        self.assertIn("firmwareSha256", self.protocol)


if __name__ == "__main__":
    unittest.main()


class HardeningRegressionTests(unittest.TestCase):
    """Guards for the reliability and honesty fixes, checked statically.

    These assert on source text rather than behaviour because the dashboard is
    a dependency-free static bundle with no JS test runner in CI.
    """

    @classmethod
    def setUpClass(cls):
        cls.html = (ROOT / "index.html").read_text(encoding="utf-8")
        cls.script = "\n".join(
            (ROOT / name).read_text(encoding="utf-8")
            for name in ("app.js", "auth.js", "data.js")
        )
        cls.local_dashboard = (PROJECT_ROOT / "include" / "web_dashboard.h").read_text(encoding="utf-8")
        cls.platformio = (PROJECT_ROOT / "platformio.ini").read_text(encoding="utf-8")
        cls.firmware = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted((PROJECT_ROOT / "src").glob("*.cpp"))
        )
        cls.gitignore = (PROJECT_ROOT / ".gitignore").read_text(encoding="utf-8")

    def test_history_coverage_is_reported_to_the_reader(self):
        # The old API silently truncated a 7-day request to the newest rows.
        self.assertIn('id="chart-coverage"', self.html)
        self.assertIn("describeCoverage", self.script)
        self.assertIn("chart-coverage", self.script)

    def test_history_failure_is_visible_without_stopping_live_updates(self):
        # Current data and history use separate endpoints. A history failure
        # must not be swallowed while the current-reading poll keeps working.
        self.assertIn("showHistoryError(error)", self.script)
        self.assertIn("History request failed:", self.script)
        self.assertNotIn("fetchHistory().catch(() =>", self.script)

    def test_sampling_default_is_two_second_live_cadence(self):
        self.assertIn("MEASUREMENT_INTERVAL_SECONDS=2", self.platformio)
        self.assertIn("samplingMode = SoilSamplingMode::Live", self.firmware)

    def test_upload_ports_are_not_hardcoded_to_one_machine(self):
        self.assertNotIn("upload_port", self.platformio)
        self.assertNotIn("monitor_port", self.platformio)

    def test_gateway_supervises_itself(self):
        self.assertIn("esp_task_wdt_add", self.firmware)
        self.assertIn("SILENT_RADIO_REBOOT_MS", self.firmware)

    def test_cloud_uploads_survive_a_reboot(self):
        # A RAM-only queue lost everything buffered during an outage.
        self.assertIn("LittleFS", self.firmware)
        self.assertIn("spoolAppend", self.firmware)
        self.assertIn("board_build.filesystem = littlefs", self.platformio)

    def test_cloud_payload_remains_backward_compatible(self):
        # Optional diagnostics stay on the LAN page until the deployed API is
        # upgraded; the stable payload prevents older functions returning 422.
        self.assertIn('\\"battery_mv\\":%s,\\"battery_percent\\":null,', self.firmware)
        self.assertNotIn("char powerMeasuredValue[6]", self.firmware)

    def test_calibration_is_settable_without_reflashing_the_sensor(self):
        self.assertIn("/api/calibration", self.firmware)
        self.assertIn("/api/calibration", self.local_dashboard)
        self.assertIn("moistureFromRaw", self.firmware)
        self.assertIn('id="calCaptureDry"', self.local_dashboard)
        self.assertIn('id="calCaptureWet"', self.local_dashboard)
        self.assertIn("calCaptureSamples.length<3", self.local_dashboard)
        self.assertIn("spread>80", self.local_dashboard)
        self.assertIn("calibratedPacket.moisturePercent", self.firmware)

    def test_ota_requires_a_password(self):
        self.assertIn("ArduinoOTA.setPassword", self.firmware)
        self.assertIn("otaPassword.length() < 12", self.firmware)

    def test_sensor_applies_instant_and_five_minute_commands(self):
        sensor = (PROJECT_ROOT / "src" / "sensor_node.cpp").read_text(encoding="utf-8")
        self.assertIn("onAcknowledgementReceived", sensor)
        self.assertIn("INSTANT_INTERVAL_SECONDS = 2", sensor)
        self.assertIn("FIVE_MINUTE_INTERVAL_SECONDS = 300", sensor)
        self.assertIn("sampleIntervalSeconds", sensor)
        self.assertIn("modeChangePending", sensor)
        self.assertNotIn("esp_deep_sleep", sensor)
        self.assertNotIn("installWirelessSensorUpdate", sensor)

    def test_transcripts_cannot_be_committed(self):
        # chat.txt held the Wi-Fi password in plaintext and this repo is public.
        self.assertIn("chat.txt", self.gitignore)
