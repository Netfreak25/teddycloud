#!/usr/bin/env python3
"""Execute actual server subscription, settings-fallback and publish ownership paths."""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/mqtt_server.c").read_text(encoding="utf-8")
    names = ("mqtt_topic_match", "mqtt_passthrough_subscription_qos",
             "mqtt_passthrough_subscription_snapshot",
             "mqtt_passthrough_subscription_apply_locked",
             "mqtt_passthrough_subscription_apply", "mqtt_settings_request_pump",
             "mqtt_build_publish_packet", "mqtt_connection_publish_packet_internal")
    functions = []
    for name in names:
        match = re.search(r"^static [\w *]+\b" + name + r"\([^;]*?\n\{[\s\S]*?\n\}", source, re.M)
        if match is None:
            raise AssertionError(f"Missing server callback: {name}")
        functions.append(match.group())
    start = source.index("typedef struct {\n    char topic[256];")
    end = source.index("} MqttClientConnection;", start) + len("} MqttClientConnection;")
    declarations = "\n".join(re.findall(
        r"^#define MQTT_(?:MAX_(?:PACKET_SIZE|CONNECTIONS|SUBSCRIPTIONS)|SETTINGS_REQUEST_TIMEOUT_MS) .+$",
        source, re.M)) + "\n" + source[start:end]
    fixture = (root / "tests/test_mqtt_session_server_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* SERVER_TYPES */", declarations)
    fixture = fixture.replace("/* SERVER_CALLBACKS */", "\n\n".join(functions))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-server-") as directory:
        generated, executable = Path(directory) / "server.c", Path(directory) / "server"
        generated.write_text(fixture, encoding="utf-8")
        subprocess.run(["gcc", "-Wall", "-Werror", "-DGPL_LICENSE_TERMS_ACCEPTED",
                        "-DHTTP_SERVER_MAX_CONNECTIONS=32", "-DTRACE_NOPATH_FILE",
                        *["-I" + path for path in includes], str(generated),
                        "-o", str(executable)], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
