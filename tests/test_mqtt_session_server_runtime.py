#!/usr/bin/env python3
"""Execute production session callbacks with controlled transport boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/mqtt_server.c").read_text(encoding="utf-8")
    names = (
        "mqtt_connection_local_control_allowed", "mqtt_topic_match",
        "mqtt_connection_has_sub", "mqtt_extract_toniebox_topic_common_name",
        "mqtt_local_settings_completed", "mqtt_publish_pending_settings_desired_to_connection",
        "mqtt_settings_request_pump", "mqtt_passthrough_subscription_qos",
        "mqtt_passthrough_subscription_snapshot", "mqtt_passthrough_subscription_apply_locked",
        "mqtt_passthrough_subscription_apply", "mqtt_settings_topic_matches",
        "mqtt_passthrough_publish_completed", "mqtt_passthrough_settings_request",
    )
    functions = []
    for name in names:
        match = re.search(r"^static [\w *]+\b" + name + r"\([^;]*?\n\{[\s\S]*?\n\}", source, re.M)
        assert match is not None, name
        functions.append(match.group())
    start = source.index("typedef struct {\n    char topic[256];")
    end = source.index("} MqttClientConnection;", start) + len("} MqttClientConnection;")
    declarations = "\n".join(re.findall(
        r"^#define MQTT_(?:MAX_(?:PACKET_SIZE|CONNECTIONS|SUBSCRIPTIONS)|SETTINGS_REQUEST_TIMEOUT_MS|SETTINGS_DESIRED_MAX_ATTEMPTS|SETTINGS_DESIRED_RETRY_INTERVAL_SEC) .+$",
        source, re.M)) + "\n" + source[start:end]
    for kind, name in (("enum", "MqttToniebox2SettingId"), ("struct", "MqttSettingsDelivery")):
        match = re.search(r"typedef " + kind + r" \{[^}]+\} " + name + r";", source)
        assert match is not None, name
        declarations += "\n" + match.group()
    relay = (root / "src/tb2_mqtt_passthrough.c").read_text(encoding="utf-8")
    for name in ("tb2_mqtt_topic_filter_valid", "tb2_mqtt_utf8_valid"):
        valid = re.search(r"^(?:static )?bool_t " + name + r"\([^;]*?\n\{[\s\S]*?\n\}", relay, re.M)
        assert valid is not None, name
        functions.insert(0, valid.group())
    fixture = (root / "tests/test_mqtt_session_server_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* SERVER_TYPES */", declarations)
    fixture = fixture.replace("/* SERVER_FUNCTIONS */", "\n\n".join(functions))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-session-server-") as directory:
        generated = Path(directory) / "runtime.c"
        executable = Path(directory) / "runtime"
        generated.write_text(fixture, encoding="utf-8")
        subprocess.run(["cc", "-Wall", "-Werror", "-DGPL_LICENSE_TERMS_ACCEPTED",
                        "-DHTTP_SERVER_MAX_CONNECTIONS=32", "-DTRACE_NOPATH_FILE",
                        *["-I" + path for path in includes], str(generated),
                        "-pthread", "-o", str(executable)], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
