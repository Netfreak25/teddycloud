#!/usr/bin/env python3
"""API coverage for TB2 HTTPS and MQTT upstream settings."""

import json
import os
import time
import unittest
import urllib.error
import urllib.request


BASE_URL = os.environ.get("TEDDYCLOUD_BASE_URL", "http://127.0.0.1:80").rstrip("/")


class UpstreamSettingsApiTests(unittest.TestCase):
    CONFIGURABLE_SETTINGS = (
        "mqtt_client_upstream.port",
        "mqtt_client_upstream.hostname",
        "mqtt_client_upstream.capture_dir",
        "mqtt_client_upstream.capture_max_mib",
        "mqtt_client_upstream.enabled",
        "mqtt_client_upstream.passthrough_enabled",
        "cloud.remote_port_tb2",
        "cloud.remote_hostname_tb2",
        "cloud.tb2_capture_dir",
        "cloud.tb2_capture_max_mib",
        "cloud.tb2_capture_enabled",
        "cloud.tb2_enabled",
        "cloud.tb2_v3_enabled",
    )

    @classmethod
    def request(cls, method, path, body=None, timeout=10):
        data = None if body is None else body.encode("utf-8")
        request = urllib.request.Request(
            f"{BASE_URL}{path}",
            data=data,
            method=method,
            headers={"Content-Type": "text/plain"} if data is not None else {},
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.getcode(), response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read().decode("utf-8", errors="replace")

    @classmethod
    def wait_for_server(cls, timeout_seconds=12.0):
        deadline = time.time() + timeout_seconds
        while time.time() < deadline:
            try:
                status, _ = cls.request("GET", "/web/")
                if status == 200:
                    return
            except OSError:
                pass
            time.sleep(0.5)
        raise RuntimeError(f"TeddyCloud API not reachable at {BASE_URL}")

    @classmethod
    def get_setting(cls, name):
        status, body = cls.request("GET", f"/api/settings/get/{name}")
        if status != 200:
            raise RuntimeError(f"Could not read {name}: HTTP {status}: {body}")
        return body.strip()

    @classmethod
    def set_setting(cls, name, value):
        return cls.request("POST", f"/api/settings/set/{name}", str(value))

    @classmethod
    def trigger(cls, name):
        status, body = cls.request("GET", f"/api/{name}")
        if status != 200 or body.strip() != "OK":
            raise RuntimeError(f"{name} failed: HTTP {status}: {body}")

    @classmethod
    def setUpClass(cls):
        cls.wait_for_server()
        cls.original_values = {
            name: cls.get_setting(name) for name in cls.CONFIGURABLE_SETTINGS
        }

    @classmethod
    def tearDownClass(cls):
        for name, value in cls.original_values.items():
            cls.set_setting(name, value)
        cls.trigger("triggerWriteConfig")

    def test_defaults_and_metadata(self):
        status, body = self.request("GET", "/api/settings/getIndex?nolevel=true")
        self.assertEqual(status, 200)
        options = {entry["ID"]: entry for entry in json.loads(body)["options"]}

        mqtt_enabled = options["mqtt_client_upstream.enabled"]
        self.assertFalse(mqtt_enabled["valueInit"])
        self.assertFalse(mqtt_enabled["readOnly"])
        mqtt_passthrough = options["mqtt_client_upstream.passthrough_enabled"]
        self.assertFalse(mqtt_passthrough["valueInit"])
        self.assertTrue(mqtt_passthrough["readOnly"])
        self.assertEqual(options["mqtt_client_upstream.port"]["valueInit"], 8883)
        self.assertEqual(
            options["mqtt_client_upstream.hostname"]["valueInit"],
            "ici.tonie.cloud",
        )
        self.assertEqual(
            options["mqtt_client_upstream.capture_dir"]["valueInit"],
            "data/diagnostics/tb2-mqtt-passthrough",
        )
        self.assertEqual(options["mqtt_client_upstream.capture_max_mib"]["valueInit"], 4096)
        mqtt_forward_options = {
            name: option
            for name, option in options.items()
            if name.startswith("mqtt_client_upstream.forward.")
        }
        self.assertEqual(len(mqtt_forward_options), 63)
        self.assertTrue(all(option["valueInit"] for option in mqtt_forward_options.values()))
        self.assertNotIn("mqtt_client_upstream.block.claim", options)

        https_enabled = options["cloud.tb2_enabled"]
        self.assertFalse(https_enabled["valueInit"])
        self.assertFalse(https_enabled["readOnly"])
        self.assertEqual(https_enabled["label"], "Enable transparent TB2 HTTPS proxy")
        self.assertFalse(options["cloud.tb2_v3_enabled"]["valueInit"])
        self.assertTrue(options["cloud.tb2_capture_enabled"]["valueInit"])
        self.assertNotIn("cloud.tb2_passthrough_enabled", options)
        self.assertEqual(options["cloud.remote_port_tb2"]["valueInit"], 443)
        self.assertEqual(
            options["cloud.remote_hostname_tb2"]["valueInit"],
            "tbs2.tonie.cloud",
        )
        self.assertEqual(
            options["cloud.tb2_capture_dir"]["valueInit"],
            "data/diagnostics/tb2-https-passthrough",
        )
        self.assertEqual(options["cloud.tb2_capture_max_mib"]["valueInit"], 4096)

        tb1_ca = options["core.client_cert_tb1.file.ca"]
        tb2_ca = options["core.client_cert_tb2.file.ca"]
        self.assertEqual(tb1_ca["label"], "Client CA (TB1)")
        self.assertEqual(tb1_ca["valueInit"], "certs/client_tb1/ca.der")
        self.assertEqual(tb2_ca["label"], "Client CA (TB2)")
        self.assertEqual(tb2_ca["valueInit"], "certs/client_tb2/ca.der")
        self.assertNotEqual(tb1_ca["valueInit"], tb2_ca["valueInit"])

    def test_mqtt_passthrough_requires_upstream_and_disables_with_it(self):
        self.set_setting("mqtt_client_upstream.enabled", "false")
        status, body = self.set_setting("mqtt_client_upstream.passthrough_enabled", "true")
        self.assertEqual(status, 200)
        self.assertEqual(body.strip(), "ERROR")

        status, body = self.set_setting("mqtt_client_upstream.enabled", "true")
        self.assertEqual(status, 200)
        self.assertEqual(body.strip(), "OK")
        status, body = self.set_setting("mqtt_client_upstream.passthrough_enabled", "true")
        self.assertEqual(status, 200)
        self.assertEqual(body.strip(), "OK")

        self.set_setting("mqtt_client_upstream.enabled", "false")
        self.assertEqual(self.get_setting("mqtt_client_upstream.passthrough_enabled"), "false")

    def test_tb2_https_modes_are_mutually_exclusive(self):
        self.set_setting("cloud.tb2_enabled", "false")
        self.set_setting("cloud.tb2_v3_enabled", "false")

        status, body = self.set_setting("cloud.tb2_v3_enabled", "true")
        self.assertEqual(status, 200)
        self.assertEqual(body.strip(), "OK")
        self.assertEqual(self.get_setting("cloud.tb2_v3_enabled"), "true")
        self.assertEqual(self.get_setting("cloud.tb2_enabled"), "false")

        status, body = self.set_setting("cloud.tb2_enabled", "true")
        self.assertEqual(status, 200)
        self.assertEqual(body.strip(), "OK")
        self.assertEqual(self.get_setting("cloud.tb2_enabled"), "true")
        self.assertEqual(self.get_setting("cloud.tb2_v3_enabled"), "false")

        status, body = self.request("GET", "/api/tb2-https-upstream/status")
        self.assertEqual(status, 200)
        payload = json.loads(body)
        self.assertTrue(payload["enabled"])
        self.assertTrue(payload["passthrough_enabled"])
        self.assertEqual(payload["mode"], "transparent")
        self.assertIn(payload["state"], ("ready", "connecting", "tunneling", "success", "error"))
        self.assertIn("v3", payload)
        self.assertIn("transparent", payload)
        self.assertIn("mode_counts", payload)
        self.assertEqual(payload["hostname"], self.get_setting("cloud.remote_hostname_tb2"))

        status, body = self.set_setting("cloud.tb2_passthrough_enabled", "false")
        self.assertEqual(status, 200)
        self.assertEqual(body.strip(), "ERROR")

    def test_configurable_endpoints_survive_reload(self):
        persisted = {
            "mqtt_client_upstream.port": "18883",
            "mqtt_client_upstream.hostname": "ici-upstream-test.invalid",
            "mqtt_client_upstream.capture_dir": "data/diagnostics/mqtt-test-capture",
            "mqtt_client_upstream.capture_max_mib": "128",
            "mqtt_client_upstream.enabled": "true",
            "mqtt_client_upstream.passthrough_enabled": "true",
            "cloud.remote_port_tb2": "14443",
            "cloud.remote_hostname_tb2": "tb2-https-test.invalid",
            "cloud.tb2_capture_dir": "data/diagnostics/tb2-test-capture",
            "cloud.tb2_capture_max_mib": "128",
            "cloud.tb2_capture_enabled": "false",
            "cloud.tb2_enabled": "true",
            "cloud.tb2_v3_enabled": "false",
        }
        runtime_only = {
            "mqtt_client_upstream.port": "18884",
            "mqtt_client_upstream.hostname": "ici-runtime-only.invalid",
            "mqtt_client_upstream.capture_dir": "data/diagnostics/mqtt-runtime-only",
            "mqtt_client_upstream.capture_max_mib": "129",
            "mqtt_client_upstream.enabled": "true",
            "mqtt_client_upstream.passthrough_enabled": "false",
            "cloud.remote_port_tb2": "14444",
            "cloud.remote_hostname_tb2": "tb2-runtime-only.invalid",
            "cloud.tb2_capture_dir": "data/diagnostics/tb2-runtime-only",
            "cloud.tb2_capture_max_mib": "129",
            "cloud.tb2_capture_enabled": "true",
            "cloud.tb2_enabled": "false",
            "cloud.tb2_v3_enabled": "true",
        }

        for name, value in persisted.items():
            status, body = self.set_setting(name, value)
            self.assertEqual(status, 200)
            self.assertEqual(body.strip(), "OK")
        self.trigger("triggerWriteConfig")

        for name, value in runtime_only.items():
            status, body = self.set_setting(name, value)
            self.assertEqual(status, 200)
            self.assertEqual(body.strip(), "OK")
        self.trigger("triggerReloadConfig")

        for name, value in persisted.items():
            self.assertEqual(self.get_setting(name), value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
