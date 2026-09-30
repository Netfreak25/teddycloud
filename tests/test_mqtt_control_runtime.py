#!/usr/bin/env python3
"""Execute real server policy/sender blocks with only their I/O boundaries stubbed.

Run with Python and gcc on Linux/WSL. All generated files stay in a temporary
directory; no test-only hooks or alternate policy implementation enter the server.
"""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/mqtt_server.c").read_text(encoding="utf-8")
    names = (
        "mqtt_connection_local_control_allowed",
        "mqtt_connection_matches_box_overlay", "mqtt_validate_json_payload",
        "mqtt_connection_topic_id", "mqtt_app_control_topic",
        "mqtt_is_toniebox2_overlay", "mqtt_control_connection_available",
        "mqtt_payload_hash", "mqtt_record_app_control_stl",
        "mqtt_control_delivery_free", "mqtt_control_json_bytes",
        "mqtt_control_delivery_commit", "mqtt_local_control_completed",
        "mqtt_control_delivery_create", "mqtt_submit_control_locked",
        "mqtt_server_publish_app_control_for_overlay", "mqtt_build_publish_packet",
        "mqtt_server_publish_ping_for_overlay", "mqtt_server_publish_shutdown_for_overlay",
        "mqtt_local_settings_completed",
    )
    functions = []
    for name in names:
        match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\([^;]*?\n\{[\s\S]*?\n\}", source, re.M)
        if match is None:
            raise AssertionError(f"Server function missing: {name}")
        functions.append(match.group())
    start = source.index("typedef struct {\n    char topic[256];")
    end = source.index("} MqttClientConnection;", start) + len("} MqttClientConnection;")
    declarations = "\n".join(re.findall(
        r"^#define MQTT_MAX_(?:PACKET_SIZE|CONNECTIONS|SUBSCRIPTIONS) .+$", source, re.M))
    declarations += "\n" + source[start:end]
    declarations += "\n" + re.search(
        r"typedef struct \{\n    MqttClientConnection \*conn;\n"
        r"    tb2_mqtt_passthrough_session_t \*session;\n    char command\[16\];[\s\S]*?\n\} MqttControlDelivery;",
        source).group()
    for typename in ("MqttAppControlStlState", "MqttAppControlPingState"):
        declarations += "\n" + re.search(r"typedef struct \{\n    bool_t valid;[^{]*?\n\} " + typename + ";", source).group()
    declarations += "\n" + re.search(r"typedef enum \{\n    MQTT_TB2_SETTING_MAX_VOLUME[\s\S]*?\n\} MqttToniebox2SettingId;", source).group()
    declarations += "\n" + re.search(r"typedef struct \{\n    MqttClientConnection \*conn;\n    tb2_mqtt_passthrough_session_t \*session;\n    uint64_t revisions[\s\S]*?\n\} MqttSettingsDelivery;", source).group()
    fixture = (root / "tests/test_mqtt_control_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* SERVER_TYPES */", declarations)
    fixture = fixture.replace("/* SERVER_FUNCTIONS */", "\n\n".join(functions))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-control-") as directory:
        generated = Path(directory) / "runtime.c"
        executable = Path(directory) / "runtime"
        generated.write_text(fixture, encoding="utf-8")
        subprocess.run(["gcc", "-Wall", "-Werror", "-DGPL_LICENSE_TERMS_ACCEPTED",
                        "-DHTTP_SERVER_MAX_CONNECTIONS=32", "-DTRACE_NOPATH_FILE",
                        *["-I" + path for path in includes], str(generated),
                        "cJSON/cJSON.c",
                        "-pthread", "-lm", "-o", str(executable)], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
