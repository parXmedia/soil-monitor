from html.parser import HTMLParser
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


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
        cls.script = (ROOT / "app.js").read_text(encoding="utf-8")
        cls.config = (ROOT / "config.js").read_text(encoding="utf-8")
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
            "access-key", "forget-key-button"
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

    def test_read_key_is_session_only_and_sent_as_bearer(self):
        self.assertIn("sessionStorage.setItem(ACCESS_KEY", self.script)
        self.assertNotIn("localStorage", self.script)
        self.assertIn("Authorization: `Bearer ${this.readToken}`", self.script)
        self.assertIn("sessionStorage.removeItem(ACCESS_KEY)", self.script)

    def test_null_optional_telemetry_stays_unavailable(self):
        self.assertRegex(
            self.script,
            r'function finiteNumber\(value, fallback = null\) \{\s*'
            r'if \(value === null \|\| value === undefined \|\| value === ""\) '
            r'return fallback;',
        )

    def test_freshness_is_bounded_and_last_update_is_visible(self):
        self.assertIn("staleAfterMs: 360000", self.config)
        self.assertIn('freshness.fresh ? "Up to date" : "Stale data"', self.script)
        self.assertIn("formatLastUpdate(reading.timestamp)", self.script)
        self.assertNotIn('freshness.fresh ? "Live"', self.script)


if __name__ == "__main__":
    unittest.main()
