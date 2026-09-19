#!/usr/bin/env python3
"""Run the production MQTT session parser/QoS flow with transport-only stubs.

Like test_mqtt_control_runtime.py, this compiles the actual production function
bodies, not a protocol reimplementation. Generated files remain temporary.
"""
from pathlib import Path
import re
import subprocess
import tempfile


FUNCTION = re.compile(
    r"^(?:static )?[\w *]+\b(tb2_mqtt_\w+)\([^;]*?\n\{[\s\S]*?\n\}", re.M
)


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/tb2_mqtt_passthrough.c").read_text(encoding="utf-8")
    fixture = (root / "tests/test_tb2_mqtt_session_runtime.c").read_text(encoding="utf-8")
    functions = {match[1]: match.group() for match in FUNCTION.finditer(source)}
    boundaries = {match[1] for match in FUNCTION.finditer(fixture)}
    required = {
        "tb2_mqtt_process_stream", "tb2_mqtt_cloud_reset", "tb2_mqtt_complete_write",
        "tb2_mqtt_runtime_update", "tb2_mqtt_passthrough_add_runtime_status",
        "tb2_mqtt_passthrough_is_established", "tb2_mqtt_passthrough_is_upstream_connected",
        "tb2_mqtt_passthrough_task", "tb2_mqtt_build_cloud_connect",
        "tb2_mqtt_passthrough_reserve_local_packet_id",
        "tb2_mqtt_passthrough_release_local_packet_id",
        "tb2_mqtt_passthrough_write_local_publish",
    }
    selected = set()
    while required:
        name = required.pop()
        if name in selected or name in boundaries:
            continue
        if name not in functions:
            raise AssertionError(f"Session function missing: {name}")
        selected.add(name)
        required.update(callee for callee in re.findall(r"\b(tb2_mqtt_\w+)\s*\(", functions[name])
                        if callee in functions and callee not in selected)
    bodies = [body for name, body in functions.items() if name in selected]
    prototypes = "\n".join(body[:body.index("\n{")] + ";" for body in bodies)
    declarations = source[:FUNCTION.search(source).start()]
    generated = fixture.replace("/* SESSION_DECLARATIONS */", declarations)
    generated = generated.replace("/* SESSION_PROTOTYPES */", prototypes)
    generated = generated.replace("/* SESSION_FUNCTIONS */", "\n\n".join(bodies))
    includes = ["include", "include/protobuf-c", "src/proto", "src/cyclone/common",
                "src/cyclone/cyclone_tcp", "cyclone/common", "cyclone/cyclone_ssl",
                "cyclone/cyclone_tcp", "cyclone/cyclone_crypto", "cJSON"]
    with tempfile.TemporaryDirectory(prefix="tc-mqtt-session-") as directory:
        test_source = Path(directory) / "session.c"
        executable = Path(directory) / "session"
        test_source.write_text(generated, encoding="utf-8")
        subprocess.run(["gcc", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        "-DGPL_LICENSE_TERMS_ACCEPTED", "-DHTTP_SERVER_MAX_CONNECTIONS=32",
                        "-DTRACE_NOPATH_FILE", *["-I" + path for path in includes], str(test_source),
                        "src/mqtt_forward_filter.c", "src/mqtt_response_history.c",
                        "cyclone/cyclone_crypto/hash/sha256.c", "cyclone/common/cpu_endian.c", "cJSON/cJSON.c",
                        "-pthread", "-lm", "-o", str(executable)],
                       cwd=root, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
