#!/usr/bin/env python3
"""Exercise production freshness delivery with controlled clock and I/O boundaries.

Queue, sender, pump and acknowledgement functions are compiled verbatim from
the current checkout. Run using Python and gcc on Linux/WSL.
"""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/mqtt_server.c").read_text(encoding="utf-8")
    names = (
        "mqtt_fresh_tonies_reset_connection", "mqtt_uid_to_ruid",
        "mqtt_fresh_tonies_publish_state", "mqtt_mark_fresh_tonies_pending",
        "mqtt_clear_fresh_tonies_pending", "mqtt_fresh_tonie_find",
        "mqtt_fresh_tonies_sync_connection", "mqtt_fresh_tonies_next",
        "mqtt_fresh_tonies_cache_contains", "mqtt_build_fresh_tonie_payload",
        "mqtt_send_fresh_tonie", "mqtt_fresh_tonies_pump",
        "mqtt_handle_fresh_tonies_puback", "mqtt_server_publish_fresh_tonie_for_overlay",
    )
    functions = []
    for name in names:
        match = re.search(
            r"^(?:static )?[\w *]+\b" + name + r"\([^;]*?\n\{[\s\S]*?\n\}",
            source, re.M,
        )
        if match is None:
            raise AssertionError(f"Freshness function missing: {name}")
        functions.append(match.group())
    start = source.index("typedef struct {\n    char topic[256];")
    end = source.index("} MqttClientConnection;", start) + len("} MqttClientConnection;")
    state = re.search(
        r"typedef struct \{\n    bool_t pending;[\s\S]*?\n\} MqttFreshToniesPublishState;",
        source,
    )
    if state is None:
        raise AssertionError("Freshness overlay state missing")
    declarations = "\n".join(re.findall(
        r"^#define MQTT_(?:MAX_(?:PACKET_SIZE|CONNECTIONS|SUBSCRIPTIONS)|"
        r"FRESH_TONIES_\w+|MILLISECONDS_PER_SECOND) .+$", source, re.M,
    )) + "\n" + source[start:end] + "\n" + state.group()
    fixture = (root / "tests/test_mqtt_fresh_tonies_runtime.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* SERVER_TYPES */", declarations)
    fixture = fixture.replace("/* SERVER_FUNCTIONS */", "\n\n".join(functions))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-fresh-delivery-") as directory:
        generated = Path(directory) / "fresh_delivery.c"
        executable = Path(directory) / "fresh_delivery"
        generated.write_text(fixture, encoding="utf-8")
        subprocess.run([
            "gcc", "-Wall", "-Wextra", "-Werror", "-DGPL_LICENSE_TERMS_ACCEPTED",
            "-DHTTP_SERVER_MAX_CONNECTIONS=32", "-DTRACE_NOPATH_FILE",
            *["-I" + path for path in includes], str(generated), "-o", str(executable),
        ], cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
